/* glacier — display server: seat + libinput + WM + CPU compositor (see server.h). */
#define _GNU_SOURCE
#include "server.h"
#include "platform.h"
#include "seat.h"
#include "input.h"
#include "window.h"
#include "wm.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TITLEBAR_H 28
#define COLOR_DESKTOP   0x00101820u
#define COLOR_TB_FOCUS  0x003a6fd0u
#define COLOR_TB_PLAIN  0x00606060u

static volatile sig_atomic_t running = 1;
static void on_sigint(int s) { (void)s; running = 0; }

/* 11x16 arrow: 'X' outline, '.' fill, ' ' transparent. The real cursor is a
 * KMS hardware-cursor plane (next step); this is the CPU-fallback software
 * cursor so the pointer is visible from frame zero regardless. */
static const char *const CURSOR[16] = {
	"X          ", "XX         ", "X.X        ", "X..X       ",
	"X...X      ", "X....X     ", "X.....X    ", "X......X   ",
	"X.......X  ", "X....XXXXX ", "X..X..X    ", "X.X X..X   ",
	"XX   X..X  ", "X    X..X  ", "      X..X ", "       XX  ",
};

/* ---- CPU compositor -------------------------------------------------- */

static void fill_rect(struct dumb_fb *fb, int x, int y, int w, int h,
                      uint32_t color)
{
	int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
	int x1 = x + w > fb->w ? fb->w : x + w;
	int y1 = y + h > fb->h ? fb->h : y + h;
	for (int yy = y0; yy < y1; yy++) {
		uint32_t *row = (uint32_t *)(fb->map + (size_t)yy * fb->stride);
		for (int xx = x0; xx < x1; xx++)
			row[xx] = color;
	}
}

static void draw_cursor(struct dumb_fb *fb, int cx, int cy)
{
	for (int r = 0; r < 16; r++) {
		for (int c = 0; CURSOR[r][c]; c++) {
			char p = CURSOR[r][c];
			if (p == ' ')
				continue;
			int x = cx + c, y = cy + r;
			if (x < 0 || x >= fb->w || y < 0 || y >= fb->h)
				continue;
			uint32_t *row = (uint32_t *)(fb->map + (size_t)y * fb->stride);
			row[x] = (p == 'X') ? 0x00000000u : 0x00ffffffu;
		}
	}
}

static void composite(struct dumb_fb *fb, struct wm *wm)
{
	struct window_stack *s = wm->stack;
	fill_rect(fb, 0, 0, fb->w, fb->h, COLOR_DESKTOP);

	/* Bottom-to-top so higher windows occlude lower ones. */
	for (int i = 0; i < s->count; i++) {
		struct window *w = &s->windows[i];
		if (!w->mapped)
			continue;
		if (w->role == WIN_DESKTOP) {
			fill_rect(fb, w->x, w->y, w->w, w->h, w->color);
			continue;
		}
		/* Server-side decoration: a title bar the server draws for every
		 * window, so chrome is identical no matter which transport
		 * delivered the content. Focused windows get the accent colour. */
		bool focused = (w->id == s->focus_id);
		fill_rect(fb, w->x, w->y, w->w, TITLEBAR_H,
		          focused ? COLOR_TB_FOCUS : COLOR_TB_PLAIN);
		fill_rect(fb, w->x, w->y + TITLEBAR_H, w->w, w->h - TITLEBAR_H,
		          w->color);
	}
	draw_cursor(fb, wm->cursor_x, wm->cursor_y);
}

/* ---- input routing --------------------------------------------------- */

struct loop_ctx { struct wm *wm; bool dirty; };

static void on_input(const struct input_event *ev, void *user)
{
	struct loop_ctx *c = user;
	switch (ev->kind) {
	case INPUT_MOTION:
		wm_pointer_motion(c->wm, ev->dx, ev->dy);
		c->dirty = true;
		break;
	case INPUT_BUTTON:
		wm_pointer_button(c->wm, ev->button, ev->pressed);
		c->dirty = true;
		break;
	case INPUT_KEY:
		if (ev->pressed && ev->keysym == 0xff1bu) { /* XKB_KEY_Escape */
			running = 0;
			break;
		}
		if (wm_key(c->wm, ev->keysym, ev->pressed, ev->alt_down))
			c->dirty = true;
		break;
	}
}

/* ---- demo scene (until a client transport lands in Phase 3) ---------- */

static void seed_windows(struct window_stack *s, int sw, int sh)
{
	window_create(s, WIN_DESKTOP, 0, 0, sw, sh, 0x00204060u, "desktop");
	window_create(s, WIN_NORMAL, sw / 6, sh / 6, 520, 360, 0x00d8d8d8u,
	              "Window A");
	uint32_t b = window_create(s, WIN_NORMAL, sw / 3, sh / 3, 480, 320,
	                           0x00eeeeeeu, "Window B");
	window_focus(s, b);
}

/* ---- main loop ------------------------------------------------------- */

int server_run(int argc, char **argv)
{
	signal(SIGINT, on_sigint);
	signal(SIGTERM, on_sigint);
	const char *explicit_path = argc > 1 ? argv[1] : NULL;

	struct seat seat;
	if (seat_open(&seat) != 0)
		return 1;
	LOG_INFO("seat opened (active=%d)", (int)seat_active(&seat));

	/* Pick the DRM device. The /dev/dri/cardN number is NOT stable —
	 * card0 may not exist (e.g. a VM where the GPU enumerates as card1) —
	 * so unless an explicit path was given we probe card0..15 and take the
	 * first KMS-capable node, preferring one with a connected connector
	 * (same policy as drm_open()). The open must go through seatd: as the
	 * unprivileged session we may not open the DRM master ourselves. */
	char path[64];
	int drm_fd = -1, drm_dev = -1;

	if (explicit_path) {
		snprintf(path, sizeof(path), "%s", explicit_path);
		LOG_INFO("server_run: starting (device=%s)", path);
		drm_dev = seat_open_device(&seat, path, &drm_fd);
		if (drm_dev < 0 || drm_set_caps(drm_fd) != 0) {
			seat_close(&seat);
			return 1;
		}
	} else {
		LOG_INFO("server_run: probing /dev/dri/card0..15 for a KMS device");
		int fb_dev = -1, fb_fd = -1;
		char fb_path[64] = {0};
		for (int i = 0; i < 16 && drm_dev < 0; i++) {
			char cand[64];
			snprintf(cand, sizeof(cand), "/dev/dri/card%d", i);
			if (access(cand, F_OK) != 0)
				continue;
			int fd = -1;
			int dev = seat_open_device(&seat, cand, &fd);
			if (dev < 0)
				continue;
			if (drm_set_caps(fd) != 0) {
				seat_close_device(&seat, dev);
				continue;
			}
			if (drm_has_connected(fd)) {
				drm_dev = dev;
				drm_fd = fd;
				snprintf(path, sizeof(path), "%s", cand);
				LOG_INFO("using %s (connected)", cand);
			} else if (fb_dev < 0) {
				fb_dev = dev;
				fb_fd = fd;
				snprintf(fb_path, sizeof(fb_path), "%s", cand);
				LOG_INFO("fallback candidate %s (no connected connector)", cand);
			} else {
				seat_close_device(&seat, dev);
			}
		}
		if (drm_dev < 0) {
			if (fb_dev < 0) {
				LOG_ERR("no KMS-capable DRM device found in /dev/dri");
				seat_close(&seat);
				return 1;
			}
			drm_dev = fb_dev;
			drm_fd = fb_fd;
			snprintf(path, sizeof(path), "%s", fb_path);
			LOG_INFO("using %s (fallback, no connected connector)", path);
		} else if (fb_dev >= 0) {
			seat_close_device(&seat, fb_dev); /* found connected; drop fallback */
		}
	}
	LOG_INFO("opened %s via seat", path);

	struct kms k;
	if (kms_setup(drm_fd, &k) != 0) {
		seat_close_device(&seat, drm_dev);
		seat_close(&seat);
		return 1;
	}

	struct dumb_fb fb[2];
	int have_fb = 0, rc = 0;
	for (; have_fb < 2; have_fb++)
		if (dumb_fb_create(drm_fd, k.mode.hdisplay, k.mode.vdisplay,
		                   &fb[have_fb]) != 0) {
			rc = 1;
			goto cleanup;
		}
	LOG_INFO("framebuffers ready (2x %dx%d, XRGB8888)",
	         k.mode.hdisplay, k.mode.vdisplay);

	struct window_stack stack;
	window_stack_init(&stack);
	seed_windows(&stack, k.mode.hdisplay, k.mode.vdisplay);

	struct wm wm;
	wm_init(&wm, &stack, k.mode.hdisplay, k.mode.vdisplay);

	/* Input is best-effort. A display server with no usable mouse/keyboard
	 * should still light up the desktop, not exit and get respawned by the
	 * greeter into a black screen. If libinput can't bind (no devices, seat
	 * grabbed elsewhere, udev empty), warn and run on rather than die. */
	struct input *input = input_create(&seat);
	if (!input)
		LOG_WARN("input unavailable; running without pointer/keyboard");

	struct loop_ctx ctx = { .wm = &wm, .dirty = true };
	bool master = false, need_modeset = false;
	int front = 0;

	LOG_INFO("glacier wm: %dx%d — Alt-Tab to switch, drag to move, Esc to quit",
	         k.mode.hdisplay, k.mode.vdisplay);

	while (running) {
		if (seat_active(&seat) && !master) {
			int sm = drmSetMaster(drm_fd);
			LOG_INFO("acquiring DRM master: drmSetMaster=%d%s", sm,
			         sm == 0 ? " (ok)" : " — continuing (libseat may already hold it)");
			master = true;
			need_modeset = true;
			ctx.dirty = true;
		} else if (!seat_active(&seat) && master) {
			drmDropMaster(drm_fd);
			master = false;
		}

		if (master && ctx.dirty) {
			int back = front ^ 1;
			composite(&fb[back], &wm);

			drmModeAtomicReq *req = drmModeAtomicAlloc();
			uint32_t flags = 0;
			if (need_modeset) {
				kms_atomic_modeset(req, &k);
				kms_atomic_plane(req, &k, fb[back].fb_id);
				flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
			} else {
				atomic_add(req, k.plane_id, &k.plane_props, "FB_ID",
				           fb[back].fb_id);
			}
			int cr = drmModeAtomicCommit(drm_fd, req, flags, NULL);
			if (cr != 0)
				LOG_WARN("atomic commit failed (flags=0x%x modeset=%d): %s",
				         flags, (int)need_modeset, strerror(errno));
			else if (need_modeset)
				LOG_INFO("first modeset commit OK — desktop should now be on screen");
			drmModeAtomicFree(req);
			front = back;
			need_modeset = false;
			ctx.dirty = false;
		}

		struct pollfd pfd[2] = {
			{ .fd = seat_fd(&seat), .events = POLLIN },
			{ .fd = input ? input_fd(input) : -1, .events = POLLIN },
		};
		if (poll(pfd, 2, 1000) < 0 && errno != EINTR)
			break;
		if (pfd[0].revents & POLLIN)
			seat_dispatch(&seat, 0);
		if (input && (pfd[1].revents & POLLIN))
			input_dispatch(input, on_input, &ctx);
	}

	if (master)
		drmDropMaster(drm_fd);
	input_destroy(input);
cleanup:
	for (int i = 0; i < have_fb; i++)
		dumb_fb_destroy(drm_fd, &fb[i]);
	kms_finish(&k);
	seat_close_device(&seat, drm_dev);
	seat_close(&seat);
	return rc;
}
