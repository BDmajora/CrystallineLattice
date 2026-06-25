/* glacier — Transport B: the Wayland compatibility frontend (see wayland.h).
 *
 * A small, self-implemented Wayland server over libwayland-server. Each
 * xdg_toplevel becomes a server-owned glacier window (role NORMAL) in the same
 * window_stack the CrystallineLattice transport and the CPU compositor use, so
 * foreign Linux apps wear the server's Windows title bar and cursor. CSD is
 * forbidden via xdg-decoration; wl_pointer.set_cursor is ignored. */
#define _GNU_SOURCE
#include "wayland.h"
#include "window.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#include <wayland-server.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-shell-server-protocol.h"
#include "xdg-decoration-server-protocol.h"

#define COMPOSITOR_VERSION 4
#define SEAT_VERSION       5     /* wl_pointer.frame is since v5 */
#define OUTPUT_VERSION     3
#define XDG_WM_BASE_VERSION 3
#define DECORATION_VERSION 1

struct fe_surface {
	struct wl_frontend *fe;
	struct wl_resource *surface;       /* wl_surface */
	struct wl_resource *xdg_surface;   /* or NULL */
	struct wl_resource *xdg_toplevel;  /* or NULL */
	struct wl_resource *decoration;    /* zxdg_toplevel_decoration_v1, or NULL */

	bool attached;                     /* an attach is pending for next commit */
	struct wl_resource *pending_buffer;/* attached buffer (may be NULL = unmap) */
	bool initial_configured;           /* sent the first xdg configure */

	uint32_t *content;                 /* XRGB copy of the client buffer */
	int cw, ch;                        /* content size */

	uint32_t server_id;                /* glacier window id, 0 = unmapped */
	char title[64];

	struct wl_list link;               /* fe->surfaces */
};

struct wl_frontend {
	struct wl_display *display;
	struct wl_event_loop *loop;
	const char *socket;
	struct window_stack *stack;
	int sw, sh;
	bool dirty;
	int cascade;                       /* placement counter for new windows */

	struct wl_list surfaces;           /* fe_surface.link */
	struct wl_list pointers;           /* wl_pointer resources (via get_link) */
	struct wl_list keyboards;          /* wl_keyboard resources */
	struct wl_list frame_callbacks;    /* pending wl_callback resources */

	struct xkb_context *xkb;
	struct xkb_keymap *keymap;
	struct xkb_state *xkb_state;
	int keymap_fd;
	size_t keymap_size;

	uint32_t pointer_focus;            /* glacier window id under the pointer */
	uint32_t kbd_focus;                /* glacier window id with kbd focus */
	int last_px, last_py;              /* last surface-local pointer position */
};

static uint32_t now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static struct fe_surface *surface_by_window(struct wl_frontend *fe, uint32_t id)
{
	struct fe_surface *s;
	if (!id)
		return NULL;
	wl_list_for_each(s, &fe->surfaces, link)
		if (s->server_id == id)
			return s;
	return NULL;
}

/* ---- frame callbacks (throttle client repaint to our cadence) -------- */

static void frame_callback_destroy(struct wl_resource *r)
{
	wl_list_remove(wl_resource_get_link(r));
}

static void fire_frame_callbacks(struct wl_frontend *fe)
{
	struct wl_resource *cb, *tmp;
	uint32_t t = now_ms();
	wl_resource_for_each_safe(cb, tmp, &fe->frame_callbacks) {
		wl_callback_send_done(cb, t);  /* destructor unlinks on destroy */
		wl_resource_destroy(cb);
	}
}

/* ---- wl_surface ------------------------------------------------------ */

static void surface_unmap(struct fe_surface *s)
{
	if (s->server_id) {
		window_destroy(s->fe->stack, s->server_id);
		s->server_id = 0;
		s->fe->dirty = true;
	}
	free(s->content);
	s->content = NULL;
	s->cw = s->ch = 0;
}

static void copy_shm_buffer(struct fe_surface *s, struct wl_shm_buffer *shmb)
{
	int w = wl_shm_buffer_get_width(shmb);
	int h = wl_shm_buffer_get_height(shmb);
	int stride = wl_shm_buffer_get_stride(shmb);
	if (w <= 0 || h <= 0)
		return;

	uint32_t *dst = s->content;
	if (s->cw != w || s->ch != h) {
		dst = realloc(s->content, (size_t)w * h * 4);
		if (!dst)
			return;
		s->content = dst;
		s->cw = w;
		s->ch = h;
	}
	wl_shm_buffer_begin_access(shmb);
	const uint8_t *src = wl_shm_buffer_get_data(shmb);
	for (int y = 0; y < h; y++)
		memcpy(dst + (size_t)y * w, src + (size_t)y * stride, (size_t)w * 4);
	wl_shm_buffer_end_access(shmb);
}

static void surface_map_or_update(struct fe_surface *s)
{
	struct wl_frontend *fe = s->fe;
	if (s->server_id) {
		struct window *win = window_by_id(fe->stack, s->server_id);
		if (win) {
			win->buf = s->content;
			win->buf_w = s->cw;
			win->buf_h = s->ch;
			win->buf_stride = s->cw * 4;
			win->w = s->cw;
			win->h = s->ch + DECOR_TITLEBAR_H;
		}
		fe->dirty = true;
		return;
	}
	/* First content: reparent as a server-owned NORMAL window. */
	int n = fe->cascade++ % 8;
	int x = 120 + 40 * n, y = 90 + 40 * n;
	uint32_t id = window_create(fe->stack, WIN_NORMAL, x, y,
	                            s->cw, s->ch + DECOR_TITLEBAR_H, 0x00202020u,
	                            s->title[0] ? s->title : "Wayland");
	if (!id)
		return;
	s->server_id = id;
	struct window *win = window_by_id(fe->stack, id);
	win->buf = s->content;
	win->buf_w = s->cw;
	win->buf_h = s->ch;
	win->buf_stride = s->cw * 4;
	window_focus(fe->stack, id);
	fe->dirty = true;
	LOG_INFO("wayland: mapped toplevel \"%s\" (%dx%d) → window %u",
	         s->title[0] ? s->title : "?", s->cw, s->ch, id);
}

static void surf_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void surf_attach(struct wl_client *c, struct wl_resource *r,
                        struct wl_resource *buffer, int32_t x, int32_t y)
{
	(void)c; (void)x; (void)y;
	struct fe_surface *s = wl_resource_get_user_data(r);
	s->attached = true;
	s->pending_buffer = buffer;
}
static void surf_frame(struct wl_client *c, struct wl_resource *r, uint32_t cb)
{
	struct fe_surface *s = wl_resource_get_user_data(r);
	struct wl_resource *res = wl_resource_create(c, &wl_callback_interface, 1, cb);
	if (res) {
		wl_resource_set_implementation(res, NULL, NULL, frame_callback_destroy);
		wl_list_insert(&s->fe->frame_callbacks, wl_resource_get_link(res));
	}
}
static void surf_noop_region(struct wl_client *c, struct wl_resource *r,
                             struct wl_resource *region) { (void)c; (void)r; (void)region; }
static void surf_damage(struct wl_client *c, struct wl_resource *r,
                        int32_t x, int32_t y, int32_t w, int32_t h)
{ (void)c; (void)r; (void)x; (void)y; (void)w; (void)h; }
static void surf_set_i32(struct wl_client *c, struct wl_resource *r, int32_t v)
{ (void)c; (void)r; (void)v; }
static void surf_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y)
{ (void)c; (void)r; (void)x; (void)y; }

static void surf_commit(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	struct fe_surface *s = wl_resource_get_user_data(r);

	/* xdg-shell: after the client's initial (bufferless) commit, the server
	 * sends the first configure; the client then acks, attaches and commits. */
	if (s->xdg_surface && !s->initial_configured) {
		struct wl_array states;
		wl_array_init(&states);
		if (s->xdg_toplevel)
			xdg_toplevel_send_configure(s->xdg_toplevel, 0, 0, &states);
		xdg_surface_send_configure(s->xdg_surface,
		                           wl_display_next_serial(s->fe->display));
		wl_array_release(&states);
		s->initial_configured = true;
		return;
	}

	if (s->attached) {
		struct wl_resource *buf = s->pending_buffer;
		s->attached = false;
		s->pending_buffer = NULL;
		if (!buf) {
			surface_unmap(s);
		} else {
			struct wl_shm_buffer *shmb = wl_shm_buffer_get(buf);
			if (shmb) {
				copy_shm_buffer(s, shmb);
				wl_buffer_send_release(buf);
				if (s->content && s->xdg_toplevel)
					surface_map_or_update(s);
			} else {
				/* dma-buf import is the next step; release so the client
				 * isn't wedged waiting on a buffer we can't sample yet. */
				wl_buffer_send_release(buf);
			}
		}
	}
	fire_frame_callbacks(s->fe);
}

static const struct wl_surface_interface surface_impl = {
	.destroy = surf_destroy,
	.attach = surf_attach,
	.damage = surf_damage,
	.frame = surf_frame,
	.set_opaque_region = surf_noop_region,
	.set_input_region = surf_noop_region,
	.commit = surf_commit,
	.set_buffer_transform = surf_set_i32,
	.set_buffer_scale = surf_set_i32,
	.damage_buffer = surf_damage,
	.offset = surf_offset,
};

static void surface_resource_destroy(struct wl_resource *r)
{
	struct fe_surface *s = wl_resource_get_user_data(r);
	surface_unmap(s);
	wl_list_remove(&s->link);
	free(s);
}

/* ---- wl_region (no-op geometry) -------------------------------------- */

static void region_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void region_op(struct wl_client *c, struct wl_resource *r,
                      int32_t x, int32_t y, int32_t w, int32_t h)
{ (void)c; (void)r; (void)x; (void)y; (void)w; (void)h; }
static const struct wl_region_interface region_impl = {
	.destroy = region_destroy, .add = region_op, .subtract = region_op,
};

/* ---- wl_compositor ---------------------------------------------------- */

static void comp_create_surface(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
	struct wl_frontend *fe = wl_resource_get_user_data(r);
	struct fe_surface *s = calloc(1, sizeof(*s));
	if (!s) { wl_client_post_no_memory(c); return; }
	s->fe = fe;
	struct wl_resource *res = wl_resource_create(c, &wl_surface_interface,
	                                             wl_resource_get_version(r), id);
	if (!res) { free(s); wl_client_post_no_memory(c); return; }
	s->surface = res;
	wl_resource_set_implementation(res, &surface_impl, s, surface_resource_destroy);
	wl_list_insert(&fe->surfaces, &s->link);
}
static void comp_create_region(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
	(void)r;
	struct wl_resource *res = wl_resource_create(c, &wl_region_interface, 1, id);
	if (res)
		wl_resource_set_implementation(res, &region_impl, NULL, NULL);
}
static const struct wl_compositor_interface compositor_impl = {
	.create_surface = comp_create_surface,
	.create_region = comp_create_region,
};
static void compositor_bind(struct wl_client *c, void *data, uint32_t ver, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(c, &wl_compositor_interface, ver, id);
	if (r)
		wl_resource_set_implementation(r, &compositor_impl, data, NULL);
}

/* ---- xdg_toplevel ---------------------------------------------------- */

static void tl_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void tl_set_title(struct wl_client *c, struct wl_resource *r, const char *title)
{
	(void)c;
	struct fe_surface *s = wl_resource_get_user_data(r);
	snprintf(s->title, sizeof(s->title), "%s", title ? title : "");
	if (s->server_id) {
		struct window *win = window_by_id(s->fe->stack, s->server_id);
		if (win)
			snprintf(win->title, sizeof(win->title), "%s", s->title);
	}
}
static void tl_set_str(struct wl_client *c, struct wl_resource *r, const char *v)
{ (void)c; (void)r; (void)v; }
static void tl_set_res(struct wl_client *c, struct wl_resource *r, struct wl_resource *x)
{ (void)c; (void)r; (void)x; }                 /* set_parent / set_fullscreen */
static void tl_noop(struct wl_client *c, struct wl_resource *r) { (void)c; (void)r; }
static void tl_show_menu(struct wl_client *c, struct wl_resource *r,
                         struct wl_resource *seat, uint32_t serial, int32_t x, int32_t y)
{ (void)c; (void)r; (void)seat; (void)serial; (void)x; (void)y; }
static void tl_move(struct wl_client *c, struct wl_resource *r,
                    struct wl_resource *seat, uint32_t serial)
{ (void)c; (void)r; (void)seat; (void)serial; }
static void tl_resize(struct wl_client *c, struct wl_resource *r,
                      struct wl_resource *seat, uint32_t serial, uint32_t edges)
{ (void)c; (void)r; (void)seat; (void)serial; (void)edges; }
static void tl_set_geom(struct wl_client *c, struct wl_resource *r, int32_t a, int32_t b)
{ (void)c; (void)r; (void)a; (void)b; }
static const struct xdg_toplevel_interface toplevel_impl = {
	.destroy = tl_destroy,
	.set_parent = tl_set_res,
	.set_title = tl_set_title,
	.set_app_id = tl_set_str,
	.show_window_menu = tl_show_menu,
	.move = tl_move,
	.resize = tl_resize,
	.set_max_size = tl_set_geom,
	.set_min_size = tl_set_geom,
	.set_maximized = tl_noop,
	.unset_maximized = tl_noop,
	.set_fullscreen = tl_set_res,
	.unset_fullscreen = tl_noop,
	.set_minimized = tl_noop,
};
static void toplevel_resource_destroy(struct wl_resource *r)
{
	struct fe_surface *s = wl_resource_get_user_data(r);
	if (s) {
		s->xdg_toplevel = NULL;
		surface_unmap(s);
	}
}

/* ---- xdg_surface ----------------------------------------------------- */

static void xs_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void xs_get_toplevel(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
	struct fe_surface *s = wl_resource_get_user_data(r);
	struct wl_resource *res = wl_resource_create(c, &xdg_toplevel_interface,
	                                             wl_resource_get_version(r), id);
	if (!res) { wl_client_post_no_memory(c); return; }
	s->xdg_toplevel = res;
	wl_resource_set_implementation(res, &toplevel_impl, s, toplevel_resource_destroy);
}
static void xs_get_popup(struct wl_client *c, struct wl_resource *r, uint32_t id,
                         struct wl_resource *parent, struct wl_resource *positioner)
{
	(void)r; (void)parent; (void)positioner;
	/* Popups are not composited yet; hand back an inert object so clients that
	 * probe for them don't error out. */
	struct wl_resource *res = wl_resource_create(c, &xdg_popup_interface, 1, id);
	if (res)
		wl_resource_set_implementation(res, NULL, NULL, NULL);
}
static void xs_set_geometry(struct wl_client *c, struct wl_resource *r,
                            int32_t x, int32_t y, int32_t w, int32_t h)
{ (void)c; (void)r; (void)x; (void)y; (void)w; (void)h; }
static void xs_ack_configure(struct wl_client *c, struct wl_resource *r, uint32_t serial)
{ (void)c; (void)r; (void)serial; }
static const struct xdg_surface_interface xdg_surface_impl = {
	.destroy = xs_destroy,
	.get_toplevel = xs_get_toplevel,
	.get_popup = xs_get_popup,
	.set_window_geometry = xs_set_geometry,
	.ack_configure = xs_ack_configure,
};
static void xdg_surface_resource_destroy(struct wl_resource *r)
{
	struct fe_surface *s = wl_resource_get_user_data(r);
	if (s)
		s->xdg_surface = NULL;
}

/* ---- xdg_wm_base ----------------------------------------------------- */

static void wm_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void wm_create_positioner(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
	(void)r;
	struct wl_resource *res = wl_resource_create(c, &xdg_positioner_interface,
	                                             wl_resource_get_version(r), id);
	if (res)
		wl_resource_set_implementation(res, NULL, NULL, NULL);
}
static void wm_get_xdg_surface(struct wl_client *c, struct wl_resource *r, uint32_t id,
                               struct wl_resource *surface)
{
	struct fe_surface *s = wl_resource_get_user_data(surface);
	struct wl_resource *res = wl_resource_create(c, &xdg_surface_interface,
	                                             wl_resource_get_version(r), id);
	if (!res) { wl_client_post_no_memory(c); return; }
	s->xdg_surface = res;
	wl_resource_set_implementation(res, &xdg_surface_impl, s,
	                               xdg_surface_resource_destroy);
}
static void wm_pong(struct wl_client *c, struct wl_resource *r, uint32_t serial)
{ (void)c; (void)r; (void)serial; }
static const struct xdg_wm_base_interface xdg_wm_base_impl = {
	.destroy = wm_destroy,
	.create_positioner = wm_create_positioner,
	.get_xdg_surface = wm_get_xdg_surface,
	.pong = wm_pong,
};
static void xdg_wm_base_bind(struct wl_client *c, void *data, uint32_t ver, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(c, &xdg_wm_base_interface, ver, id);
	if (r)
		wl_resource_set_implementation(r, &xdg_wm_base_impl, data, NULL);
}

/* ---- xdg-decoration: force server-side decorations ------------------- */

static void deco_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void deco_force_ssd(struct wl_resource *r)
{
	/* Whatever the client asks, the answer is SERVER_SIDE — glacier draws the
	 * Windows title bar for every window, so CSD is forbidden. */
	zxdg_toplevel_decoration_v1_send_configure(
		r, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}
static void deco_set_mode(struct wl_client *c, struct wl_resource *r, uint32_t mode)
{ (void)c; (void)mode; deco_force_ssd(r); }
static void deco_unset_mode(struct wl_client *c, struct wl_resource *r)
{ (void)c; deco_force_ssd(r); }
static const struct zxdg_toplevel_decoration_v1_interface decoration_impl = {
	.destroy = deco_destroy,
	.set_mode = deco_set_mode,
	.unset_mode = deco_unset_mode,
};
static void decomgr_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void decomgr_get(struct wl_client *c, struct wl_resource *r, uint32_t id,
                        struct wl_resource *toplevel)
{
	(void)r;
	struct fe_surface *s = wl_resource_get_user_data(toplevel);
	struct wl_resource *res = wl_resource_create(c,
		&zxdg_toplevel_decoration_v1_interface, DECORATION_VERSION, id);
	if (!res) { wl_client_post_no_memory(c); return; }
	if (s)
		s->decoration = res;
	wl_resource_set_implementation(res, &decoration_impl, s, NULL);
	deco_force_ssd(res);   /* announce SSD immediately */
}
static const struct zxdg_decoration_manager_v1_interface decomgr_impl = {
	.destroy = decomgr_destroy,
	.get_toplevel_decoration = decomgr_get,
};
static void decoration_bind(struct wl_client *c, void *data, uint32_t ver, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(c,
		&zxdg_decoration_manager_v1_interface, ver, id);
	if (r)
		wl_resource_set_implementation(r, &decomgr_impl, data, NULL);
}

/* ---- wl_output ------------------------------------------------------- */

static void output_bind(struct wl_client *c, void *data, uint32_t ver, uint32_t id)
{
	struct wl_frontend *fe = data;
	struct wl_resource *r = wl_resource_create(c, &wl_output_interface, ver, id);
	if (!r)
		return;
	wl_resource_set_implementation(r, NULL, fe, NULL);
	wl_output_send_geometry(r, 0, 0, 0, 0, WL_OUTPUT_SUBPIXEL_UNKNOWN,
	                        "glacier", "virtual", WL_OUTPUT_TRANSFORM_NORMAL);
	wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
	                    fe->sw, fe->sh, 60000);
	if (ver >= WL_OUTPUT_SCALE_SINCE_VERSION)
		wl_output_send_scale(r, 1);
	if (ver >= WL_OUTPUT_DONE_SINCE_VERSION)
		wl_output_send_done(r);
}

/* ---- wl_seat (pointer + keyboard) ------------------------------------ */

static void seat_resource_unlink(struct wl_resource *r)
{
	wl_list_remove(wl_resource_get_link(r));
}
static void ptr_set_cursor(struct wl_client *c, struct wl_resource *r, uint32_t serial,
                           struct wl_resource *surf, int32_t hx, int32_t hy)
{
	/* Ignored: glacier owns the cursor plane globally. */
	(void)c; (void)r; (void)serial; (void)surf; (void)hx; (void)hy;
}
static void res_release(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wl_pointer_interface pointer_impl = {
	.set_cursor = ptr_set_cursor, .release = res_release,
};
static const struct wl_keyboard_interface keyboard_impl = { .release = res_release };
static const struct wl_touch_interface touch_impl = { .release = res_release };

static void seat_get_pointer(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
	struct wl_frontend *fe = wl_resource_get_user_data(r);
	struct wl_resource *res = wl_resource_create(c, &wl_pointer_interface,
	                                             wl_resource_get_version(r), id);
	if (!res) { wl_client_post_no_memory(c); return; }
	wl_resource_set_implementation(res, &pointer_impl, fe, seat_resource_unlink);
	wl_list_insert(&fe->pointers, wl_resource_get_link(res));
}
static void seat_get_keyboard(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
	struct wl_frontend *fe = wl_resource_get_user_data(r);
	struct wl_resource *res = wl_resource_create(c, &wl_keyboard_interface,
	                                             wl_resource_get_version(r), id);
	if (!res) { wl_client_post_no_memory(c); return; }
	wl_resource_set_implementation(res, &keyboard_impl, fe, seat_resource_unlink);
	wl_list_insert(&fe->keyboards, wl_resource_get_link(res));
	if (fe->keymap_fd >= 0)
		wl_keyboard_send_keymap(res, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
		                        fe->keymap_fd, fe->keymap_size);
	if (wl_resource_get_version(res) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION)
		wl_keyboard_send_repeat_info(res, 25, 600);
}
static void seat_get_touch(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
	struct wl_resource *res = wl_resource_create(c, &wl_touch_interface,
	                                             wl_resource_get_version(r), id);
	if (res)
		wl_resource_set_implementation(res, &touch_impl, NULL, NULL);
}
static void seat_release(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wl_seat_interface seat_impl = {
	.get_pointer = seat_get_pointer,
	.get_keyboard = seat_get_keyboard,
	.get_touch = seat_get_touch,
	.release = seat_release,
};
static void seat_bind(struct wl_client *c, void *data, uint32_t ver, uint32_t id)
{
	struct wl_frontend *fe = data;
	struct wl_resource *r = wl_resource_create(c, &wl_seat_interface, ver, id);
	if (!r)
		return;
	wl_resource_set_implementation(r, &seat_impl, fe, NULL);
	wl_seat_send_capabilities(r, WL_SEAT_CAPABILITY_POINTER |
	                             WL_SEAT_CAPABILITY_KEYBOARD);
	if (ver >= WL_SEAT_NAME_SINCE_VERSION)
		wl_seat_send_name(r, "seat0");
}

/* ---- input routing into the focused surface -------------------------- */

static void pointer_frame_all(struct wl_frontend *fe, struct wl_client *client)
{
	struct wl_resource *p;
	wl_resource_for_each(p, &fe->pointers)
		if (wl_resource_get_client(p) == client &&
		    wl_resource_get_version(p) >= WL_POINTER_FRAME_SINCE_VERSION)
			wl_pointer_send_frame(p);
}

void wl_frontend_pointer_motion(struct wl_frontend *fe, int gx, int gy)
{
	/* Which Wayland window is under the pointer, and is it over content
	 * (below the server title bar)? The title bar belongs to the WM. */
	struct window *hit = window_at(fe->stack, gx, gy);
	struct fe_surface *s = hit ? surface_by_window(fe, hit->id) : NULL;
	int lx = 0, ly = 0;
	bool over_content = false;
	if (s) {
		lx = gx - hit->x;
		ly = gy - (hit->y + DECOR_TITLEBAR_H);
		over_content = ly >= 0 && ly < s->ch && lx >= 0 && lx < s->cw;
	}
	uint32_t target = over_content ? hit->id : 0;

	if (target != fe->pointer_focus) {
		struct fe_surface *old = surface_by_window(fe, fe->pointer_focus);
		uint32_t serial = wl_display_next_serial(fe->display);
		struct wl_resource *p;
		if (old)
			wl_resource_for_each(p, &fe->pointers)
				if (wl_resource_get_client(p) == wl_resource_get_client(old->surface))
					wl_pointer_send_leave(p, serial, old->surface);
		if (s && over_content) {
			serial = wl_display_next_serial(fe->display);
			wl_resource_for_each(p, &fe->pointers)
				if (wl_resource_get_client(p) == wl_resource_get_client(s->surface)) {
					wl_pointer_send_enter(p, serial, s->surface,
					                      wl_fixed_from_int(lx), wl_fixed_from_int(ly));
					pointer_frame_all(fe, wl_resource_get_client(s->surface));
				}
		}
		fe->pointer_focus = target;
	}
	if (over_content && s) {
		fe->last_px = lx;
		fe->last_py = ly;
		struct wl_resource *p;
		wl_resource_for_each(p, &fe->pointers)
			if (wl_resource_get_client(p) == wl_resource_get_client(s->surface))
				wl_pointer_send_motion(p, now_ms(),
				                       wl_fixed_from_int(lx), wl_fixed_from_int(ly));
		pointer_frame_all(fe, wl_resource_get_client(s->surface));
	}
}

void wl_frontend_pointer_button(struct wl_frontend *fe, uint32_t button, bool pressed)
{
	struct fe_surface *s = surface_by_window(fe, fe->pointer_focus);
	if (!s)
		return;
	uint32_t serial = wl_display_next_serial(fe->display);
	struct wl_resource *p;
	wl_resource_for_each(p, &fe->pointers)
		if (wl_resource_get_client(p) == wl_resource_get_client(s->surface))
			wl_pointer_send_button(p, serial, now_ms(), button,
				pressed ? WL_POINTER_BUTTON_STATE_PRESSED
				        : WL_POINTER_BUTTON_STATE_RELEASED);
	pointer_frame_all(fe, wl_resource_get_client(s->surface));
}

static void send_modifiers(struct wl_frontend *fe, struct wl_client *client)
{
	if (!fe->xkb_state)
		return;
	uint32_t dep = xkb_state_serialize_mods(fe->xkb_state, XKB_STATE_MODS_DEPRESSED);
	uint32_t lat = xkb_state_serialize_mods(fe->xkb_state, XKB_STATE_MODS_LATCHED);
	uint32_t lok = xkb_state_serialize_mods(fe->xkb_state, XKB_STATE_MODS_LOCKED);
	uint32_t grp = xkb_state_serialize_layout(fe->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
	uint32_t serial = wl_display_next_serial(fe->display);
	struct wl_resource *k;
	wl_resource_for_each(k, &fe->keyboards)
		if (wl_resource_get_client(k) == client)
			wl_keyboard_send_modifiers(k, serial, dep, lat, lok, grp);
}

void wl_frontend_keyboard_key(struct wl_frontend *fe, uint32_t keycode, bool pressed)
{
	if (fe->xkb_state)
		xkb_state_update_key(fe->xkb_state, keycode + 8,
		                     pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
	struct fe_surface *s = surface_by_window(fe, fe->kbd_focus);
	if (!s)
		return;
	struct wl_client *client = wl_resource_get_client(s->surface);
	uint32_t serial = wl_display_next_serial(fe->display);
	struct wl_resource *k;
	wl_resource_for_each(k, &fe->keyboards)
		if (wl_resource_get_client(k) == client)
			wl_keyboard_send_key(k, serial, now_ms(), keycode,
				pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
				        : WL_KEYBOARD_KEY_STATE_RELEASED);
	send_modifiers(fe, client);
}

void wl_frontend_keyboard_focus(struct wl_frontend *fe, uint32_t window_id)
{
	struct fe_surface *next = surface_by_window(fe, window_id);
	uint32_t target = next ? window_id : 0;
	if (target == fe->kbd_focus)
		return;

	struct fe_surface *old = surface_by_window(fe, fe->kbd_focus);
	struct wl_resource *k;
	if (old) {
		uint32_t serial = wl_display_next_serial(fe->display);
		wl_resource_for_each(k, &fe->keyboards)
			if (wl_resource_get_client(k) == wl_resource_get_client(old->surface))
				wl_keyboard_send_leave(k, serial, old->surface);
	}
	if (next) {
		uint32_t serial = wl_display_next_serial(fe->display);
		struct wl_array keys;
		wl_array_init(&keys);
		wl_resource_for_each(k, &fe->keyboards)
			if (wl_resource_get_client(k) == wl_resource_get_client(next->surface))
				wl_keyboard_send_enter(k, serial, next->surface, &keys);
		wl_array_release(&keys);
		send_modifiers(fe, wl_resource_get_client(next->surface));
	}
	fe->kbd_focus = target;
}

/* ---- keymap fd for wl_keyboard --------------------------------------- */

static void build_keymap(struct wl_frontend *fe)
{
	fe->keymap_fd = -1;
	fe->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!fe->xkb)
		return;
	fe->keymap = xkb_keymap_new_from_names(fe->xkb, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!fe->keymap)
		return;
	fe->xkb_state = xkb_state_new(fe->keymap);
	char *str = xkb_keymap_get_as_string(fe->keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
	if (!str)
		return;
	size_t len = strlen(str) + 1;
	int fd = memfd_create("glacier-keymap", MFD_CLOEXEC);
	if (fd >= 0 && write(fd, str, len) == (ssize_t)len) {
		fe->keymap_fd = fd;
		fe->keymap_size = len;
	} else if (fd >= 0) {
		close(fd);
	}
	free(str);
}

/* ---- lifecycle ------------------------------------------------------- */

struct wl_frontend *wl_frontend_create(struct window_stack *stack, int sw, int sh)
{
	struct wl_frontend *fe = calloc(1, sizeof(*fe));
	if (!fe)
		return NULL;
	fe->stack = stack;
	fe->sw = sw;
	fe->sh = sh;
	wl_list_init(&fe->surfaces);
	wl_list_init(&fe->pointers);
	wl_list_init(&fe->keyboards);
	wl_list_init(&fe->frame_callbacks);

	fe->display = wl_display_create();
	if (!fe->display)
		goto fail;
	fe->loop = wl_display_get_event_loop(fe->display);
	if (wl_display_init_shm(fe->display) != 0)
		goto fail;
	fe->socket = wl_display_add_socket_auto(fe->display);
	if (!fe->socket)
		goto fail;

	build_keymap(fe);

	if (!wl_global_create(fe->display, &wl_compositor_interface,
	                      COMPOSITOR_VERSION, fe, compositor_bind) ||
	    !wl_global_create(fe->display, &wl_seat_interface,
	                      SEAT_VERSION, fe, seat_bind) ||
	    !wl_global_create(fe->display, &wl_output_interface,
	                      OUTPUT_VERSION, fe, output_bind) ||
	    !wl_global_create(fe->display, &xdg_wm_base_interface,
	                      XDG_WM_BASE_VERSION, fe, xdg_wm_base_bind) ||
	    !wl_global_create(fe->display, &zxdg_decoration_manager_v1_interface,
	                      DECORATION_VERSION, fe, decoration_bind))
		goto fail;

	LOG_INFO("wayland: frontend up on $WAYLAND_DISPLAY=%s", fe->socket);
	return fe;

fail:
	wl_frontend_destroy(fe);
	return NULL;
}

int wl_frontend_fd(struct wl_frontend *fe)
{
	return wl_event_loop_get_fd(fe->loop);
}

bool wl_frontend_dispatch(struct wl_frontend *fe)
{
	wl_event_loop_dispatch(fe->loop, 0);
	wl_display_flush_clients(fe->display);
	bool d = fe->dirty;
	fe->dirty = false;
	return d;
}

void wl_frontend_flush(struct wl_frontend *fe)
{
	wl_display_flush_clients(fe->display);
}

const char *wl_frontend_socket(struct wl_frontend *fe)
{
	return fe->socket;
}

void wl_frontend_destroy(struct wl_frontend *fe)
{
	if (!fe)
		return;
	if (fe->keymap_fd >= 0)
		close(fe->keymap_fd);
	if (fe->xkb_state)
		xkb_state_unref(fe->xkb_state);
	if (fe->keymap)
		xkb_keymap_unref(fe->keymap);
	if (fe->xkb)
		xkb_context_unref(fe->xkb);
	if (fe->display)
		wl_display_destroy(fe->display);
	free(fe);
}
