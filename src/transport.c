/* CrystallineLattice — server-side transport (see transport.h).
 *
 * One epoll fd fans in the listen socket plus every client. A client's
 * requests become server-owned windows; the shell keeps authority over
 * placement, z-order and focus. Buffer fds arrive via SCM_RIGHTS and the
 * shm/CPU path mmaps them straight into the window for the compositor.
 *
 * Crash-resilience (DESIGN.md §3.3): a client dying — EOF or a malformed
 * datagram — drops just that client and reaps its windows; the desktop and
 * every other client survive. */
#define _GNU_SOURCE
#include "transport.h"
#include "protocol.h"
#include "window.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define CL_MAX_CLIENTS 16

/* A client's window: its wire id, the server window it maps to, and the
 * mmap'd buffer the transport owns on its behalf. */
struct cl_win {
	uint32_t client_wid;     /* 0 ⇒ free slot */
	uint32_t server_id;
	void    *buf;            /* mmap base, or NULL */
	size_t   buf_len;        /* munmap length */
};

struct cl_client {
	int fd;                  /* -1 ⇒ free slot */
	bool hello;              /* handshake complete */
	struct cl_win wins[WIN_MAX];
};

struct transport {
	struct window_stack *stack;
	int sw, sh;
	int epoll_fd;
	int listen_fd;           /* -1 until transport_listen() */
	char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	struct cl_client clients[CL_MAX_CLIENTS];
	uint32_t focused_server_id; /* last focus reported to clients (CL_FOCUS) */
};

/* ---- small helpers --------------------------------------------------- */

static enum win_role role_from_wire(uint32_t r)
{
	switch (r) {
	case CL_ROLE_DESKTOP: return WIN_DESKTOP;
	case CL_ROLE_TASKBAR: return WIN_TASKBAR;
	case CL_ROLE_TRAY:    return WIN_TRAY;
	case CL_ROLE_MENU:    return WIN_MENU;
	case CL_ROLE_TOOLTIP: return WIN_TOOLTIP;
	default:              return WIN_NORMAL;
	}
}

static void close_fds(int *fds, int nfds)
{
	for (int i = 0; i < nfds; i++)
		close(fds[i]);
}

static struct cl_win *win_find(struct cl_client *c, uint32_t client_wid)
{
	if (!client_wid)
		return NULL;
	for (int i = 0; i < WIN_MAX; i++)
		if (c->wins[i].client_wid == client_wid)
			return &c->wins[i];
	return NULL;
}

static struct cl_win *win_alloc(struct cl_client *c)
{
	for (int i = 0; i < WIN_MAX; i++)
		if (c->wins[i].client_wid == 0)
			return &c->wins[i];
	return NULL;
}

static void win_drop_buffer(struct cl_win *w, struct window_stack *stack)
{
	if (w->buf) {
		struct window *win = window_by_id(stack, w->server_id);
		if (win)
			win->buf = NULL;
		munmap(w->buf, w->buf_len);
		w->buf = NULL;
		w->buf_len = 0;
	}
}

/* Defined in the input-routing section; used by on_create_window for the
 * initial focus and by transport_update_focus for later transitions. */
static void notify_focus(struct transport *t, uint32_t new_focus);

/* ---- server → client events ------------------------------------------ */

static void send_welcome(struct transport *t, int fd)
{
	struct cl_welcome m = {
		.type = CL_WELCOME, .magic = CL_MAGIC, .version = CL_VERSION,
		.screen_w = (uint32_t)t->sw, .screen_h = (uint32_t)t->sh,
	};
	cl_send(fd, &m, sizeof(m), NULL, 0);
}

/* ---- request handlers ------------------------------------------------ */

static void on_create_window(struct transport *t, struct cl_client *c,
                             const struct cl_create_window *m)
{
	if (win_find(c, m->wid)) {
		LOG_WARN("transport: duplicate window id %u from client", m->wid);
		return;
	}
	struct cl_win *cw = win_alloc(c);
	if (!cw) {
		LOG_WARN("transport: client window table full");
		return;
	}
	int w = m->w ? (int)m->w : 320;
	int h = m->h ? (int)m->h : 240;
	enum win_role role = role_from_wire(m->role);
	uint32_t id = window_create(t->stack, role, m->x, m->y, w, h,
	                            0x00202020u, "client");
	if (!id) {
		LOG_WARN("transport: window stack full");
		return;
	}
	cw->client_wid = m->wid;
	cw->server_id = id;

	/* Server policy by role: a new NORMAL window takes focus (and raises);
	 * the DESKTOP (wallpaper) sinks to the bottom; taskbar/tray/menus are
	 * created on top (window_create already adds there) and don't take focus. */
	if (role == WIN_NORMAL)
		window_focus(t->stack, id);
	else if (role == WIN_DESKTOP)
		window_lower(t->stack, id);

	struct window *win = window_by_id(t->stack, id);
	struct cl_configure cfg = {
		.type = CL_CONFIGURE, .wid = m->wid,
		.x = win->x, .y = win->y, .w = (uint32_t)win->w, .h = (uint32_t)win->h,
	};
	cl_send(c->fd, &cfg, sizeof(cfg), NULL, 0);
	/* A new NORMAL window took focus above; emit the CL_FOCUS transition now
	 * (focus-lost to any previously focused client, focus-gained to this one).
	 * Later focus changes (Alt-Tab, click) flow through transport_update_focus
	 * from the server loop, which shares this same path. */
	if (role == WIN_NORMAL)
		notify_focus(t, id);

	LOG_INFO("transport: window %u (role=%d %dx%d) → server id %u",
	         m->wid, (int)role, w, h, id);
}

static void on_set_title(struct transport *t, struct cl_client *c,
                         const struct cl_set_title *m)
{
	struct cl_win *cw = win_find(c, m->wid);
	if (!cw)
		return;
	struct window *win = window_by_id(t->stack, cw->server_id);
	if (!win)
		return;
	char title[CL_TITLE_MAX];
	memcpy(title, m->title, sizeof(title));
	title[sizeof(title) - 1] = '\0';
	snprintf(win->title, sizeof(win->title), "%s", title);
}

/* COMMIT consumes the passed fds. fds[0] = buffer, fds[1] = optional fence. */
static void on_commit(struct transport *t, struct cl_client *c,
                      const struct cl_commit *m, int *fds, int nfds)
{
	const struct cl_buffer *b = &m->buffer;
	struct cl_win *cw = win_find(c, m->wid);

	/* Close the render fence: the CPU/shm path samples synchronously, so we
	 * don't wait on a GPU fence yet — the GL path (next phase) will. */
	if (b->has_fence && nfds >= 2) {
		close(fds[1]);
		nfds = 1;
	}
	if (nfds < 1) {
		LOG_WARN("transport: COMMIT without a buffer fd");
		close_fds(fds, nfds);
		return;
	}
	int buf_fd = fds[0];

	if (!cw || b->kind != CL_BUF_SHM) {
		if (b->kind != CL_BUF_SHM)
			LOG_WARN("transport: dma-buf import not yet implemented (CPU path)");
		close(buf_fd);
		return;
	}
	struct window *win = window_by_id(t->stack, cw->server_id);
	if (!win || b->format != CL_FORMAT_XRGB8888 || b->stride == 0 ||
	    b->height == 0) {
		LOG_WARN("transport: rejecting buffer (fmt=0x%x %ux%u stride=%u)",
		         b->format, b->width, b->height, b->stride);
		close(buf_fd);
		return;
	}
	size_t len = (size_t)b->stride * b->height;
	void *base = mmap(NULL, len, PROT_READ, MAP_SHARED, buf_fd, 0);
	close(buf_fd);
	if (base == MAP_FAILED) {
		LOG_WARN("transport: mmap of client buffer failed: %s", strerror(errno));
		return;
	}
	win_drop_buffer(cw, t->stack);   /* release the previous frame */
	cw->buf = base;
	cw->buf_len = len;
	win->buf = base;
	win->buf_w = (int)b->width;
	win->buf_h = (int)b->height;
	win->buf_stride = (int)b->stride;
}

static void on_destroy_window(struct transport *t, struct cl_client *c,
                              const struct cl_destroy_window *m)
{
	struct cl_win *cw = win_find(c, m->wid);
	if (!cw)
		return;
	win_drop_buffer(cw, t->stack);
	window_destroy(t->stack, cw->server_id);
	memset(cw, 0, sizeof(*cw));
}

/* Returns false if the client must be dropped. */
static bool dispatch(struct transport *t, struct cl_client *c,
                     const void *buf, ssize_t n, int *fds, int nfds,
                     bool *dirty)
{
	if (n < (ssize_t)sizeof(uint32_t)) {
		close_fds(fds, nfds);
		return false;
	}
	uint32_t type = *(const uint32_t *)buf;

#define NEED(structname) \
	if (n < (ssize_t)sizeof(struct structname)) { close_fds(fds, nfds); return false; }

	/* The handshake must come first. */
	if (!c->hello) {
		if (type != CL_HELLO) {
			close_fds(fds, nfds);
			return false;
		}
		NEED(cl_hello);
		const struct cl_hello *h = buf;
		uint32_t neg = h->version < CL_VERSION ? h->version : CL_VERSION;
		if (h->magic != CL_MAGIC || neg < CL_MIN_VERSION ||
		    neg < h->min_version) {
			LOG_WARN("transport: handshake rejected (magic=0x%x ver=%u min=%u)",
			         h->magic, h->version, h->min_version);
			close_fds(fds, nfds);
			return false;
		}
		c->hello = true;
		send_welcome(t, c->fd);
		close_fds(fds, nfds);
		LOG_INFO("transport: client connected (negotiated v%u)", neg);
		return true;
	}

	switch (type) {
	case CL_CREATE_WINDOW:
		NEED(cl_create_window);
		on_create_window(t, c, buf);
		*dirty = true;
		break;
	case CL_SET_TITLE:
		NEED(cl_set_title);
		on_set_title(t, c, buf);
		break;
	case CL_COMMIT:
		NEED(cl_commit);
		on_commit(t, c, buf, fds, nfds);
		*dirty = true;
		return true;   /* on_commit owns the fds */
	case CL_DESTROY_WINDOW:
		NEED(cl_destroy_window);
		on_destroy_window(t, c, buf);
		*dirty = true;
		break;
	default:
		LOG_WARN("transport: unknown message type %u", type);
		break;
	}
#undef NEED
	close_fds(fds, nfds);
	return true;
}

/* ---- client lifecycle ------------------------------------------------ */

static struct cl_client *client_by_fd(struct transport *t, int fd)
{
	for (int i = 0; i < CL_MAX_CLIENTS; i++)
		if (t->clients[i].fd == fd)
			return &t->clients[i];
	return NULL;
}

static void client_drop(struct transport *t, struct cl_client *c, bool *dirty)
{
	for (int i = 0; i < WIN_MAX; i++) {
		if (c->wins[i].client_wid) {
			win_drop_buffer(&c->wins[i], t->stack);
			window_destroy(t->stack, c->wins[i].server_id);
		}
	}
	epoll_ctl(t->epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
	close(c->fd);
	memset(c, 0, sizeof(*c));
	c->fd = -1;
	if (dirty)
		*dirty = true;
	LOG_INFO("transport: client disconnected");
}

/* Drain one client's queued datagrams. */
static void client_readable(struct transport *t, struct cl_client *c,
                            bool *dirty)
{
	for (;;) {
		union {
			uint32_t type;
			struct cl_commit commit;
			struct cl_create_window create;
			struct cl_set_title title;
			struct cl_hello hello;
			char raw[256];
		} msg;
		int fds[CL_MAX_FDS];
		int nfds = 0;
		ssize_t n = cl_recv(c->fd, &msg, sizeof(msg), fds, &nfds);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			client_drop(t, c, dirty);
			return;
		}
		if (n == 0) {                      /* orderly shutdown */
			client_drop(t, c, dirty);
			return;
		}
		if (!dispatch(t, c, &msg, n, fds, nfds, dirty)) {
			client_drop(t, c, dirty);
			return;
		}
	}
}

/* ---- public API ------------------------------------------------------ */

struct transport *transport_create(struct window_stack *stack, int sw, int sh)
{
	struct transport *t = calloc(1, sizeof(*t));
	if (!t)
		return NULL;
	t->stack = stack;
	t->sw = sw;
	t->sh = sh;
	t->listen_fd = -1;
	for (int i = 0; i < CL_MAX_CLIENTS; i++)
		t->clients[i].fd = -1;
	t->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (t->epoll_fd < 0) {
		LOG_ERR("transport: epoll_create1 failed: %s", strerror(errno));
		free(t);
		return NULL;
	}
	return t;
}

int transport_listen(struct transport *t, const char *path)
{
	char resolved[sizeof(t->path)];
	if (path)
		snprintf(resolved, sizeof(resolved), "%s", path);
	else
		cl_default_socket_path(resolved, sizeof(resolved));

	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		LOG_ERR("transport: socket failed: %s", strerror(errno));
		return -1;
	}
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", resolved);
	unlink(resolved);   /* clear a stale socket from a previous run */
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
	    listen(fd, 8) != 0) {
		LOG_ERR("transport: bind/listen on %s failed: %s", resolved,
		        strerror(errno));
		close(fd);
		return -1;
	}
	struct epoll_event ev = { .events = EPOLLIN, .data.fd = fd };
	if (epoll_ctl(t->epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0) {
		close(fd);
		return -1;
	}
	t->listen_fd = fd;
	snprintf(t->path, sizeof(t->path), "%s", resolved);
	LOG_INFO("transport: listening on %s", resolved);
	return 0;
}

int transport_add_client(struct transport *t, int fd)
{
	struct cl_client *c = client_by_fd(t, -1);
	if (!c) {
		LOG_WARN("transport: too many clients, refusing connection");
		close(fd);
		return -1;
	}
	memset(c, 0, sizeof(*c));
	c->fd = fd;
	/* The per-client drain loop reads until EAGAIN, so the fd must be
	 * non-blocking. accept4() already gives us that, but the headless test
	 * hands us a plain socketpair end — enforce the invariant here. */
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl >= 0)
		fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	struct epoll_event ev = { .events = EPOLLIN, .data.fd = fd };
	if (epoll_ctl(t->epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0) {
		close(fd);
		c->fd = -1;
		return -1;
	}
	return 0;
}

/* ---- input routing to clients (M2) ----------------------------------- */

static bool find_by_server_id(struct transport *t, uint32_t server_id,
                              struct cl_client **co, struct cl_win **wo)
{
	if (!server_id)
		return false;
	for (int i = 0; i < CL_MAX_CLIENTS; i++) {
		struct cl_client *c = &t->clients[i];
		if (c->fd < 0)
			continue;
		for (int j = 0; j < WIN_MAX; j++) {
			if (c->wins[j].client_wid &&
			    c->wins[j].server_id == server_id) {
				*co = c;
				*wo = &c->wins[j];
				return true;
			}
		}
	}
	return false;
}

/* Title-bar height for a window: NORMAL windows wear server chrome, the
 * chromeless shell surfaces (desktop/taskbar/tray/menu/tooltip) do not. The
 * client buffer's top-left maps to (w->x, w->y + titlebar), so input is sent in
 * coordinates relative to that origin. */
static int win_titlebar(const struct window *w)
{
	return w->role == WIN_NORMAL ? DECOR_TITLEBAR_H : 0;
}

void transport_pointer_motion(struct transport *t, int cursor_x, int cursor_y)
{
	struct window *w = window_at(t->stack, cursor_x, cursor_y);
	struct cl_client *c;
	struct cl_win *cw;
	int titlebar;

	if (!w)
		return;
	titlebar = win_titlebar(w);
	if (cursor_y < w->y + titlebar)
		return;                  /* over the server title bar */
	if (!find_by_server_id(t, w->id, &c, &cw))
		return;                  /* not a CrystallineLattice client window */

	struct cl_input m = {
		.type = CL_INPUT, .wid = cw->client_wid, .kind = CL_IN_MOTION,
		.x = cursor_x - w->x, .y = cursor_y - w->y - titlebar,
	};
	cl_send(c->fd, &m, sizeof(m), NULL, 0);
}

void transport_pointer_button(struct transport *t, uint32_t button, bool pressed,
                              int cursor_x, int cursor_y)
{
	struct window *w = window_at(t->stack, cursor_x, cursor_y);
	struct cl_client *c;
	struct cl_win *cw;
	int titlebar;

	if (!w)
		return;
	titlebar = win_titlebar(w);
	if (cursor_y < w->y + titlebar)
		return;                  /* title bar press is the WM's (drag) */
	if (!find_by_server_id(t, w->id, &c, &cw))
		return;

	struct cl_input m = {
		.type = CL_INPUT, .wid = cw->client_wid, .kind = CL_IN_BUTTON,
		.x = cursor_x - w->x, .y = cursor_y - w->y - titlebar,
		.code = button, .pressed = pressed,
	};
	cl_send(c->fd, &m, sizeof(m), NULL, 0);
}

void transport_keyboard_key(struct transport *t, uint32_t focus_id,
                            uint32_t keysym, bool pressed)
{
	struct cl_client *c;
	struct cl_win *cw;

	if (!find_by_server_id(t, focus_id, &c, &cw))
		return;

	struct cl_input k = {
		.type = CL_INPUT, .wid = cw->client_wid, .kind = CL_IN_KEY,
		.code = keysym, .pressed = pressed,
	};
	cl_send(c->fd, &k, sizeof(k), NULL, 0);
}

static void notify_focus(struct transport *t, uint32_t new_focus)
{
	struct cl_client *c;
	struct cl_win *cw;

	if (new_focus == t->focused_server_id)
		return;
	if (find_by_server_id(t, t->focused_server_id, &c, &cw)) {
		struct cl_focus f = { .type = CL_FOCUS, .wid = cw->client_wid,
		                      .focused = 0 };
		cl_send(c->fd, &f, sizeof(f), NULL, 0);
	}
	t->focused_server_id = new_focus;
	if (find_by_server_id(t, new_focus, &c, &cw)) {
		struct cl_focus f = { .type = CL_FOCUS, .wid = cw->client_wid,
		                      .focused = 1 };
		cl_send(c->fd, &f, sizeof(f), NULL, 0);
	}
}

void transport_update_focus(struct transport *t, uint32_t focus_id)
{
	notify_focus(t, focus_id);
}

int transport_fd(struct transport *t)
{
	return t->epoll_fd;
}

const char *transport_socket_path(struct transport *t)
{
	return t->path;
}

static void accept_pending(struct transport *t)
{
	for (;;) {
		int cfd = accept4(t->listen_fd, NULL, NULL,
		                  SOCK_CLOEXEC | SOCK_NONBLOCK);
		if (cfd < 0)
			return;        /* EAGAIN: drained */
		transport_add_client(t, cfd);
	}
}

bool transport_process(struct transport *t)
{
	struct epoll_event evs[CL_MAX_CLIENTS + 1];
	int n = epoll_wait(t->epoll_fd, evs, CL_MAX_CLIENTS + 1, 0);
	bool dirty = false;
	for (int i = 0; i < n; i++) {
		int fd = evs[i].data.fd;
		if (fd == t->listen_fd) {
			accept_pending(t);
			continue;
		}
		struct cl_client *c = client_by_fd(t, fd);
		if (!c)
			continue;
		if (evs[i].events & (EPOLLHUP | EPOLLERR))
			client_drop(t, c, &dirty);
		else if (evs[i].events & EPOLLIN)
			client_readable(t, c, &dirty);
	}
	return dirty;
}

void transport_destroy(struct transport *t)
{
	if (!t)
		return;
	for (int i = 0; i < CL_MAX_CLIENTS; i++)
		if (t->clients[i].fd >= 0)
			client_drop(t, &t->clients[i], NULL);
	if (t->listen_fd >= 0)
		close(t->listen_fd);
	if (t->path[0])
		unlink(t->path);
	if (t->epoll_fd >= 0)
		close(t->epoll_fd);
	free(t);
}
