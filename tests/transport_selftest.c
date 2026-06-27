/* Headless test for the CrystallineLattice server transport.
 *
 * No DRM, no listen socket: we make a SOCK_SEQPACKET socketpair, hand the
 * server end to the transport via transport_add_client(), write client
 * messages on the other end, drive transport_process(), and assert the
 * server-owned window_stack changed as the protocol dictates. This decouples
 * protocol/transport bugs from hardware exactly as DESIGN.md Phase 3 intends. */
#define _GNU_SOURCE
#include "protocol.h"
#include "transport.h"
#include "window.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#define SCREEN_W 1920
#define SCREEN_H 1080

/* The transport mutates the window_stack; these helpers read it back. */
static int mapped_count(struct window_stack *s)
{
	int n = 0;
	for (int i = 0; i < s->count; i++)
		if (s->windows[i].mapped)
			n++;
	return n;
}

static struct window *only_normal(struct window_stack *s)
{
	struct window *found = NULL;
	for (int i = 0; i < s->count; i++)
		if (s->windows[i].role == WIN_NORMAL) {
			assert(!found && "expected exactly one NORMAL window");
			found = &s->windows[i];
		}
	return found;
}

static void drain(struct transport *t)
{
	/* Let the server process whatever is queued. */
	for (int i = 0; i < 4; i++)
		transport_process(t);
}

int main(void)
{
	struct window_stack stack;
	window_stack_init(&stack);

	struct transport *t = transport_create(&stack, SCREEN_W, SCREEN_H);
	assert(t);

	int sv[2];
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0);
	int cli = sv[0];
	assert(transport_add_client(t, sv[1]) == 0);

	/* --- handshake --- */
	struct cl_hello hello = { .type = CL_HELLO, .magic = CL_MAGIC,
	                          .version = CL_VERSION,
	                          .min_version = CL_MIN_VERSION };
	assert(cl_send(cli, &hello, sizeof(hello), NULL, 0) == 0);
	drain(t);

	char rbuf[256];
	int fds[CL_MAX_FDS], nfds = 0;
	ssize_t n = cl_recv(cli, rbuf, sizeof(rbuf), fds, &nfds);
	assert(n >= (ssize_t)sizeof(struct cl_welcome));
	struct cl_welcome *wel = (void *)rbuf;
	assert(wel->type == CL_WELCOME);
	assert(wel->screen_w == SCREEN_W && wel->screen_h == SCREEN_H);
	uint32_t token = wel->reconnect_token;   /* for the crash-reconnect test below */
	assert(token != 0);
	printf("ok: handshake → WELCOME %ux%u (token %u)\n",
	       wel->screen_w, wel->screen_h, token);

	assert(stack.count == 0);   /* no window yet */

	/* --- create a NORMAL window --- */
	struct cl_create_window cw = { .type = CL_CREATE_WINDOW, .wid = 7,
	                               .role = CL_ROLE_NORMAL,
	                               .x = 100, .y = 50, .w = 640, .h = 480 };
	assert(cl_send(cli, &cw, sizeof(cw), NULL, 0) == 0);
	drain(t);

	assert(stack.count == 1);
	struct window *win = only_normal(&stack);
	assert(win && win->mapped);
	assert(win->x == 100 && win->y == 50 && win->w == 640 && win->h == 480);
	assert(stack.focus_id == win->id);   /* server policy: new NORMAL focuses */
	uint32_t server_id = win->id;
	printf("ok: CREATE_WINDOW → server window id %u, focused\n", server_id);

	/* server should have sent CONFIGURE + FOCUS */
	n = cl_recv(cli, rbuf, sizeof(rbuf), fds, &nfds);
	assert(n >= (ssize_t)sizeof(struct cl_configure) &&
	       ((struct cl_configure *)rbuf)->type == CL_CONFIGURE);
	n = cl_recv(cli, rbuf, sizeof(rbuf), fds, &nfds);
	assert(n >= (ssize_t)sizeof(struct cl_focus) &&
	       ((struct cl_focus *)rbuf)->type == CL_FOCUS &&
	       ((struct cl_focus *)rbuf)->focused == 1);
	printf("ok: received CONFIGURE + FOCUS\n");

	/* --- set title --- */
	struct cl_set_title st = { .type = CL_SET_TITLE, .wid = 7 };
	snprintf(st.title, sizeof(st.title), "hello");
	assert(cl_send(cli, &st, sizeof(st), NULL, 0) == 0);
	drain(t);
	assert(strcmp(window_by_id(&stack, server_id)->title, "hello") == 0);
	printf("ok: SET_TITLE applied\n");

	/* --- commit an shm buffer via SCM_RIGHTS --- */
	const int BW = 64, BH = 32, BSTRIDE = BW * 4;
	size_t bsize = (size_t)BSTRIDE * BH;
	int mfd = memfd_create("test", MFD_CLOEXEC);
	assert(mfd >= 0 && ftruncate(mfd, bsize) == 0);
	uint32_t *px = mmap(NULL, bsize, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
	assert(px != MAP_FAILED);
	for (size_t i = 0; i < bsize / 4; i++)
		px[i] = 0x00abcdefu;

	struct cl_commit cm = { .type = CL_COMMIT, .wid = 7,
		.buffer = { .kind = CL_BUF_SHM, .width = BW, .height = BH,
		            .stride = BSTRIDE, .format = CL_FORMAT_XRGB8888 } };
	assert(cl_send(cli, &cm, sizeof(cm), &mfd, 1) == 0);
	close(mfd);
	drain(t);

	win = window_by_id(&stack, server_id);
	assert(win->buf != NULL);
	assert(win->buf_w == BW && win->buf_h == BH && win->buf_stride == BSTRIDE);
	assert(win->buf[0] == 0x00abcdefu);   /* server mmap sees client pixels */
	printf("ok: COMMIT mapped a %dx%d buffer the server can read\n", BW, BH);

	/* --- client moved/resized its own window (CL_SET_GEOMETRY) --- */
	struct cl_set_geometry sg = { .type = CL_SET_GEOMETRY, .wid = 7,
	                              .x = 300, .y = 220, .w = 800, .h = 600 };
	assert(cl_send(cli, &sg, sizeof(sg), NULL, 0) == 0);
	drain(t);
	win = window_by_id(&stack, server_id);
	assert(win->x == 300 && win->y == 220 && win->w == 800 && win->h == 600);
	printf("ok: CL_SET_GEOMETRY moved/resized the window\n");

	/* --- a NORMAL app window closes immediately on disconnect (no 5s ghost) --- */
	close(cli);
	drain(t);
	assert(stack.count == 0);   /* app windows vanish at once — graceful close */
	munmap(px, bsize);
	printf("ok: app window closed immediately on disconnect\n");

	/* --- a SHELL (taskbar) window IS preserved on crash, and reclaimable --- */
	int sv2[2];
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv2) == 0);
	cli = sv2[0];
	assert(transport_add_client(t, sv2[1]) == 0);
	struct cl_hello h2 = { .type = CL_HELLO, .magic = CL_MAGIC,
	                       .version = CL_VERSION, .min_version = CL_MIN_VERSION };
	assert(cl_send(cli, &h2, sizeof(h2), NULL, 0) == 0);
	drain(t);
	n = cl_recv(cli, rbuf, sizeof(rbuf), fds, &nfds);
	assert(n >= (ssize_t)sizeof(struct cl_welcome));
	uint32_t token2 = ((struct cl_welcome *)rbuf)->reconnect_token;

	struct cl_create_window tb = { .type = CL_CREATE_WINDOW, .wid = 9,
	                               .role = CL_ROLE_TASKBAR,
	                               .x = 0, .y = 1040, .w = 1920, .h = 40 };
	assert(cl_send(cli, &tb, sizeof(tb), NULL, 0) == 0);
	drain(t);
	assert(stack.count == 1);
	printf("ok: shell (taskbar) window created\n");

	close(cli);                 /* crash the shell client */
	drain(t);
	assert(stack.count == 1);   /* shell window IS preserved (desktop survives) */
	printf("ok: shell window preserved on crash (grace period)\n");

	int sv3[2];
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv3) == 0);
	cli = sv3[0];
	assert(transport_add_client(t, sv3[1]) == 0);
	struct cl_hello h3 = { .type = CL_HELLO, .magic = CL_MAGIC,
	                       .version = CL_VERSION, .min_version = CL_MIN_VERSION,
	                       .reconnect_token = token2 };
	assert(cl_send(cli, &h3, sizeof(h3), NULL, 0) == 0);
	drain(t);
	n = cl_recv(cli, rbuf, sizeof(rbuf), fds, &nfds);
	assert(n >= (ssize_t)sizeof(struct cl_welcome) &&
	       ((struct cl_welcome *)rbuf)->type == CL_WELCOME);
	assert(stack.count == 1);   /* taskbar reclaimed, not recreated */
	printf("ok: shell window reclaimed on reconnect\n");

	close(cli);
	transport_destroy(t);
	printf("PASS: transport self-test (graceful close + shell reconnect)\n");
	return 0;
}
