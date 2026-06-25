/* glacier-client — a trivial native CrystallineLattice client.
 *
 * Per DESIGN.md Phase 3, this exists *before* winedrm.drv so protocol bugs are
 * debugged without Wine in the loop. It connects, does the v0 handshake,
 * creates one NORMAL window, fills an shm (memfd) buffer with a gradient, and
 * commits it — then reports the server-driven events it receives. The server
 * places the window and draws its Windows title bar; the client only supplies
 * content. Run `glacier wm` first, then this from any terminal in the session. */
#define _GNU_SOURCE
#include "protocol.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define WIN_W 480
#define WIN_H 320

static int connect_server(void)
{
	char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	cl_default_socket_path(path, sizeof(path));

	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		fprintf(stderr, "connect %s: %s\n", path, strerror(errno));
		close(fd);
		return -1;
	}
	fprintf(stderr, "connected to %s\n", path);
	return fd;
}

/* memfd of XRGB8888 pixels, filled with a diagonal gradient + border so it is
 * obviously client-rendered content rather than a server placeholder. */
static int make_buffer(int *out_stride)
{
	int stride = WIN_W * 4;
	size_t size = (size_t)stride * WIN_H;
	int mfd = memfd_create("glacier-client", MFD_CLOEXEC);
	if (mfd < 0 || ftruncate(mfd, size) != 0) {
		perror("memfd");
		return -1;
	}
	uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
	if (px == MAP_FAILED) {
		perror("mmap");
		close(mfd);
		return -1;
	}
	for (int y = 0; y < WIN_H; y++) {
		for (int x = 0; x < WIN_W; x++) {
			uint8_t r = (uint8_t)(x * 255 / WIN_W);
			uint8_t g = (uint8_t)(y * 255 / WIN_H);
			uint8_t b = 0x80;
			bool edge = x < 2 || y < 2 || x >= WIN_W - 2 || y >= WIN_H - 2;
			px[y * WIN_W + x] = edge ? 0x00ffffffu
			                         : ((uint32_t)r << 16 | g << 8 | b);
		}
	}
	munmap(px, size);   /* the fd carries the content; the map was just to fill */
	*out_stride = stride;
	return mfd;
}

static const char *type_name(uint32_t t)
{
	switch (t) {
	case CL_WELCOME:   return "WELCOME";
	case CL_CONFIGURE: return "CONFIGURE";
	case CL_FOCUS:     return "FOCUS";
	case CL_CLOSE:     return "CLOSE";
	case CL_INPUT:     return "INPUT";
	default:           return "?";
	}
}

int main(void)
{
	int fd = connect_server();
	if (fd < 0)
		return 1;

	struct cl_hello hello = {
		.type = CL_HELLO, .magic = CL_MAGIC,
		.version = CL_VERSION, .min_version = CL_MIN_VERSION,
	};
	cl_send(fd, &hello, sizeof(hello), NULL, 0);

	char buf[256];
	int fds[CL_MAX_FDS], nfds = 0;
	ssize_t n = cl_recv(fd, buf, sizeof(buf), fds, &nfds);
	if (n < (ssize_t)sizeof(struct cl_welcome) ||
	    ((struct cl_welcome *)buf)->type != CL_WELCOME) {
		fprintf(stderr, "handshake failed\n");
		return 1;
	}
	struct cl_welcome *w = (void *)buf;
	fprintf(stderr, "WELCOME: v%u, screen %ux%u\n", w->version, w->screen_w,
	        w->screen_h);

	int stride = 0;
	int bfd = make_buffer(&stride);
	if (bfd < 0)
		return 1;

	uint32_t wid = 1;
	struct cl_create_window cw = {
		.type = CL_CREATE_WINDOW, .wid = wid, .role = CL_ROLE_NORMAL,
		.x = 240, .y = 180, .w = WIN_W, .h = WIN_H,
	};
	cl_send(fd, &cw, sizeof(cw), NULL, 0);

	struct cl_set_title st = { .type = CL_SET_TITLE, .wid = wid };
	snprintf(st.title, sizeof(st.title), "CrystallineLattice test client");
	cl_send(fd, &st, sizeof(st), NULL, 0);

	struct cl_commit cm = {
		.type = CL_COMMIT, .wid = wid,
		.buffer = {
			.kind = CL_BUF_SHM, .width = WIN_W, .height = WIN_H,
			.stride = (uint32_t)stride, .format = CL_FORMAT_XRGB8888,
			.modifier = 0, .has_fence = 0,
		},
	};
	if (cl_send(fd, &cm, sizeof(cm), &bfd, 1) != 0) {
		perror("commit");
		return 1;
	}
	close(bfd);
	fprintf(stderr, "window committed — staying connected (Ctrl-C to close)\n");

	/* Block on server events. Exiting would drop the client and reap the
	 * window, so stay until the shell asks us to close or the link drops. */
	for (;;) {
		n = cl_recv(fd, buf, sizeof(buf), fds, &nfds);
		if (n <= 0) {
			fprintf(stderr, "disconnected\n");
			break;
		}
		uint32_t type = *(uint32_t *)buf;
		fprintf(stderr, "event: %s (%u)\n", type_name(type), type);
		if (type == CL_CLOSE)
			break;
	}
	close(fd);
	return 0;
}
