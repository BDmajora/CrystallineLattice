/* Headless test for glacier's Wayland frontend (Transport B), no DRM.
 *
 * Parent: brings up the frontend and pumps its event loop. Child: a real
 * libwayland-client that binds wl_compositor + wl_shm + xdg_wm_base, maps an
 * xdg_toplevel with an shm buffer, and stays alive. The parent then asserts
 * the frontend reparented it into a server-owned NORMAL window carrying the
 * client's pixels — the Phase 4 "native app displays as a peer" path. */
#define _GNU_SOURCE
#include "wayland.h"
#include "window.h"

#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

#define BW 200
#define BH 120
#define FILL 0x00c3a55au

/* ------------------------------- child ------------------------------- */

struct cstate {
	struct wl_compositor *comp;
	struct wl_shm *shm;
	struct xdg_wm_base *wm;
	int configured;
};

static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t s)
{ (void)d; xdg_wm_base_pong(wm, s); }
static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

static void reg_global(void *data, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t ver)
{
	struct cstate *c = data;
	if (strcmp(iface, wl_compositor_interface.name) == 0)
		c->comp = wl_registry_bind(r, name, &wl_compositor_interface,
		                           ver < 4 ? ver : 4);
	else if (strcmp(iface, wl_shm_interface.name) == 0)
		c->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
		c->wm = wl_registry_bind(r, name, &xdg_wm_base_interface,
		                         ver < 3 ? ver : 3);
		xdg_wm_base_add_listener(c->wm, &wm_listener, c);
	}
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n)
{ (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_listener = {
	reg_global, reg_remove,
};

static void xs_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
	struct cstate *c = data;
	xdg_surface_ack_configure(xs, serial);
	c->configured = 1;
}
static const struct xdg_surface_listener xs_listener = { xs_configure };

static void tl_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h,
                         struct wl_array *s) { (void)d; (void)t; (void)w; (void)h; (void)s; }
static void tl_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
static void tl_cfg_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h)
{ (void)d; (void)t; (void)w; (void)h; }
static void tl_wm_caps(void *d, struct xdg_toplevel *t, struct wl_array *c)
{ (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener tl_listener = {
	tl_configure, tl_close, tl_cfg_bounds, tl_wm_caps,
};

static struct wl_buffer *make_buffer(struct cstate *c)
{
	int stride = BW * 4;
	size_t size = (size_t)stride * BH;
	int fd = memfd_create("wl-test", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) != 0)
		return NULL;
	uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (px == MAP_FAILED)
		return NULL;
	for (size_t i = 0; i < size / 4; i++)
		px[i] = FILL;
	munmap(px, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(c->shm, fd, size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, BW, BH, stride,
	                                                  WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buf;
}

static int run_child(void)
{
	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy) { fprintf(stderr, "child: connect failed\n"); return 1; }
	struct cstate c = {0};
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_listener, &c);
	wl_display_roundtrip(dpy);
	if (!c.comp || !c.shm || !c.wm) {
		fprintf(stderr, "child: missing globals (comp=%p shm=%p wm=%p)\n",
		        (void *)c.comp, (void *)c.shm, (void *)c.wm);
		return 1;
	}
	struct wl_surface *surf = wl_compositor_create_surface(c.comp);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(c.wm, surf);
	xdg_surface_add_listener(xs, &xs_listener, &c);
	struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(tl, &tl_listener, &c);
	xdg_toplevel_set_title(tl, "test-term");
	wl_surface_commit(surf);                    /* initial, bufferless */

	while (!c.configured && wl_display_dispatch(dpy) != -1)
		;                                       /* wait for first configure */

	struct wl_buffer *buf = make_buffer(&c);
	if (!buf) { fprintf(stderr, "child: buffer failed\n"); return 1; }
	wl_surface_attach(surf, buf, 0, 0);
	wl_surface_damage(surf, 0, 0, BW, BH);
	wl_surface_frame(surf);                     /* exercise the frame-callback path */
	wl_surface_commit(surf);
	wl_display_roundtrip(dpy);
	wl_display_flush(dpy);

	for (int i = 0; i < 30; i++) {              /* stay mapped ~3s */
		wl_display_flush(dpy);
		struct pollfd pf = { .fd = wl_display_get_fd(dpy), .events = POLLIN };
		if (poll(&pf, 1, 100) > 0)
			wl_display_dispatch(dpy);
	}
	wl_display_disconnect(dpy);
	return 0;
}

/* ------------------------------- parent ------------------------------ */

static struct window *find_wayland_window(struct window_stack *s)
{
	for (int i = 0; i < s->count; i++)
		if (s->windows[i].role == WIN_NORMAL && s->windows[i].buf)
			return &s->windows[i];
	return NULL;
}

int main(void)
{
	/* add_socket_auto needs XDG_RUNTIME_DIR; provide a private one. */
	char tmpl[] = "/tmp/glacier-wl-XXXXXX";
	char *dir = mkdtemp(tmpl);
	assert(dir);
	setenv("XDG_RUNTIME_DIR", dir, 1);

	struct window_stack stack;
	window_stack_init(&stack);
	struct wl_frontend *fe = wl_frontend_create(&stack, 800, 600);
	assert(fe);

	pid_t pid = fork();
	assert(pid >= 0);
	if (pid == 0)
		_exit(run_child());

	/* Pump the frontend until the child's toplevel becomes a window. */
	struct window *win = NULL;
	for (int i = 0; i < 100 && !win; i++) {
		struct pollfd pf = { .fd = wl_frontend_fd(fe), .events = POLLIN };
		poll(&pf, 1, 50);
		wl_frontend_dispatch(fe);
		win = find_wayland_window(&stack);
	}

	int rc = 0;
	if (!win) {
		fprintf(stderr, "FAIL: frontend never mapped the client toplevel\n");
		rc = 1;
	} else {
		printf("ok: toplevel → window %u (%dx%d) title=\"%s\"\n",
		       win->id, win->w, win->h, win->title);
		assert(win->buf_w == BW && win->buf_h == BH);
		assert(win->buf[0] == FILL);   /* the client's pixels reached the server */
		assert(strcmp(win->title, "test-term") == 0);
		assert(win->h == BH + DECOR_TITLEBAR_H);   /* server adds the title bar */
		printf("ok: pixels + title + server-side title bar verified\n");
	}

	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);
	wl_frontend_destroy(fe);
	rmdir(dir);
	if (rc == 0)
		printf("PASS: wayland frontend self-test\n");
	return rc;
}
