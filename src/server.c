/* glacier — display server: seat + libinput + WM + CPU compositor (see server.h). */
#define _GNU_SOURCE
#include "server.h"
#include "platform.h"
#include "seat.h"
#include "input.h"
#include "window.h"
#include "wm.h"
#include "transport.h"
#include "wayland.h"
#include "shell.h"
#include "control.h"

#include <errno.h>
#include <fcntl.h>
#include <libudev.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TITLEBAR_H DECOR_TITLEBAR_H   /* shared with the WM (window.h) */
#define COLOR_DESKTOP   0x00101820u
#define COLOR_TB_FOCUS  0x003a6fd0u
#define COLOR_TB_PLAIN  0x00606060u

static volatile sig_atomic_t running = 1;
static void on_sigint(int s) { (void)s; running = 0; }

static int64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

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

/* Blit a client-committed XRGB8888 buffer into a window's content rect,
 * clipped to both the buffer and the framebuffer. This is the CPU/shm path;
 * the GL compositor will sample these as textures instead. */
static void blit_buffer(struct dumb_fb *fb, const struct window *w, int cy0)
{
	int dst_y0 = w->y + cy0;
	int content_h = w->h - cy0;
	int rows = content_h < w->buf_h ? content_h : w->buf_h;
	int cols = w->w < w->buf_w ? w->w : w->buf_w;
	for (int r = 0; r < rows; r++) {
		int y = dst_y0 + r;
		if (y < 0 || y >= fb->h)
			continue;
		const uint32_t *src = (const uint32_t *)
			((const uint8_t *)w->buf + (size_t)r * w->buf_stride);
		uint32_t *drow = (uint32_t *)(fb->map + (size_t)y * fb->stride);
		for (int c = 0; c < cols; c++) {
			int x = w->x + c;
			if (x < 0 || x >= fb->w)
				continue;
			drow[x] = src[c];
		}
	}
}

static void draw_cursor(struct dumb_fb *fb, int cx, int cy,
                        const struct cl_cursor *cur)
{
	if (cur && cur->hidden)
		return;

	/* A focused app's cursor shape (I-beam, resize arrows, …): ARGB blended
	 * over the desktop at the hotspot. We still own the position. */
	if (cur && cur->custom) {
		int ox = cx - cur->hotspot_x, oy = cy - cur->hotspot_y;
		for (int r = 0; r < cur->h; r++) {
			int y = oy + r;
			const uint32_t *src;
			uint32_t *row;
			if (y < 0 || y >= fb->h)
				continue;
			src = &cur->argb[r * cur->w];
			row = (uint32_t *)(fb->map + (size_t)y * fb->stride);
			for (int c = 0; c < cur->w; c++) {
				int x = ox + c;
				uint32_t px, d, r2, g2, b2;
				uint8_t a;
				if (x < 0 || x >= fb->w)
					continue;
				px = src[c];
				a = px >> 24;
				if (a == 0)
					continue;
				if (a == 0xff) {
					row[x] = px & 0x00ffffffu;
					continue;
				}
				d = row[x];
				r2 = (((px >> 16) & 0xff) * a + ((d >> 16) & 0xff) * (255 - a)) / 255;
				g2 = (((px >> 8) & 0xff) * a + ((d >> 8) & 0xff) * (255 - a)) / 255;
				b2 = ((px & 0xff) * a + (d & 0xff) * (255 - a)) / 255;
				row[x] = (r2 << 16) | (g2 << 8) | b2;
			}
		}
		return;
	}

	/* Built-in arrow, used until a client sets its own cursor. */
	for (int r = 0; r < 16; r++) {
		for (int c = 0; CURSOR[r][c]; c++) {
			char p = CURSOR[r][c];
			int x = cx + c, y = cy + r;
			if (p == ' ')
				continue;
			if (x < 0 || x >= fb->w || y < 0 || y >= fb->h)
				continue;
			uint32_t *row = (uint32_t *)(fb->map + (size_t)y * fb->stride);
			row[x] = (p == 'X') ? 0x00000000u : 0x00ffffffu;
		}
	}
}

static void composite(struct dumb_fb *fb, struct wm *wm,
                      const struct cl_cursor *cur)
{
	struct window_stack *s = wm->stack;
	fill_rect(fb, 0, 0, fb->w, fb->h, COLOR_DESKTOP);

	/* Bottom-to-top so higher windows occlude lower ones. */
	for (int i = 0; i < s->count; i++) {
		struct window *w = &s->windows[i];
		if (!w->mapped)
			continue;
		/* Glacier draws a title bar only for windows that don't decorate
		 * themselves — i.e. native Wayland apps. Wine paints its own Win32
		 * chrome, and the shell surfaces (desktop/taskbar/tray/menus/tooltips)
		 * are chromeless, so neither gets an SSD. */
		bool chrome = window_has_ssd(w);
		int cy0 = chrome ? TITLEBAR_H : 0;
		if (chrome) {
			bool focused = (w->id == s->focus_id);
			fill_rect(fb, w->x, w->y, w->w, TITLEBAR_H,
			          focused ? COLOR_TB_FOCUS : COLOR_TB_PLAIN);
		}
		/* A client buffer paints the content; until one arrives the
		 * placeholder colour fills it (and is the wallpaper for DESKTOP). */
		fill_rect(fb, w->x, w->y + cy0, w->w, w->h - cy0, w->color);
		if (w->buf)
			blit_buffer(fb, w, cy0);
	}
	draw_cursor(fb, wm->cursor_x, wm->cursor_y, cur);
}

/* ---- input routing --------------------------------------------------- */

struct loop_ctx {
	struct wm *wm;
	struct wl_frontend *wl;
	struct transport *xport;   /* CrystallineLattice clients (Transport A) */
	bool dirty;
	bool activity;             /* any input this iteration → reset the idle timer */
};

/* Ctrl+Alt+T → a terminal. The terminal is a native Wayland client, so it
 * connects back to glacier's own Wayland frontend (Transport B); we point it
 * at that socket. $GLACIER_TERMINAL overrides the default (`foot`). */
static void spawn_terminal(struct wl_frontend *wl)
{
	const char *sock = wl ? wl_frontend_socket(wl) : NULL;
	LOG_INFO("Ctrl+Alt+T → spawning a terminal (WAYLAND_DISPLAY=%s)",
	         sock ? sock : "(frontend down)");
	pid_t pid = fork();
	if (pid < 0) {
		LOG_WARN("fork for terminal failed");
		return;
	}
	if (pid == 0) {
		setsid();
		if (sock)
			setenv("WAYLAND_DISPLAY", sock, 1);
		/* Capture the terminal's own stdout/stderr so a client-side failure
		 * (missing global, font, etc.) is diagnosable instead of vanishing. */
		const char *home = getenv("HOME");
		char tlog[512];
		snprintf(tlog, sizeof(tlog), "%s/glacier-terminal.log",
		         (home && home[0]) ? home : "/tmp");
		int lfd = open(tlog, O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (lfd >= 0) {
			dup2(lfd, 1);
			dup2(lfd, 2);
			if (lfd > 2)
				close(lfd);
		}
		/* $GLACIER_TERMINAL wins; otherwise try common terminals in turn.
		 * execlp only returns on failure, so we fall through to the next. */
		const char *env = getenv("GLACIER_TERMINAL");
		if (env && env[0])
			execlp(env, env, (char *)NULL);
		static const char *const cands[] = {
			"foot", "weston-terminal", "alacritty", "kitty",
			"gnome-terminal", "xterm",
		};
		for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++)
			execlp(cands[i], cands[i], (char *)NULL);
		fprintf(stderr, "[ERR ] glacier: Ctrl+Alt+T: no terminal found "
		        "(install foot, or set $GLACIER_TERMINAL)\n");
		_exit(127);
	}
	/* Parent: SIGCHLD is SIG_IGN, so the child is auto-reaped. */
}

static void on_input(const struct input_event *ev, void *user)
{
	struct loop_ctx *c = user;
	c->activity = true;   /* any input wakes the display / resets idle blanking */
	switch (ev->kind) {
	case INPUT_MOTION:
		wm_pointer_motion(c->wm, ev->dx, ev->dy);
		if (c->wl)
			wl_frontend_pointer_motion(c->wl, c->wm->cursor_x, c->wm->cursor_y);
		if (c->xport)
			transport_pointer_motion(c->xport, c->wm->cursor_x, c->wm->cursor_y);
		c->dirty = true;
		break;
	case INPUT_MOTION_ABS:
		wm_pointer_motion_abs(c->wm, ev->ax, ev->ay);
		if (c->wl)
			wl_frontend_pointer_motion(c->wl, c->wm->cursor_x, c->wm->cursor_y);
		if (c->xport)
			transport_pointer_motion(c->xport, c->wm->cursor_x, c->wm->cursor_y);
		c->dirty = true;
		break;
	case INPUT_BUTTON:
		/* The WM focuses (and drags on the title bar); a content press also
		 * reaches the focused client (Wayland or CrystallineLattice). */
		wm_pointer_button(c->wm, ev->button, ev->pressed);
		if (c->wl)
			wl_frontend_pointer_button(c->wl, ev->button, ev->pressed);
		if (c->xport)
			transport_pointer_button(c->xport, ev->button, ev->pressed,
			                         c->wm->cursor_x, c->wm->cursor_y);
		c->dirty = true;
		break;
	case INPUT_KEY: {
		/* Track Ctrl/Alt from raw evdev keycodes, so the Ctrl+Alt+T shortcut
		 * fires even when no xkb keymap loaded (xkb modifiers would be unset).
		 * KEY_LEFTCTRL=29 RIGHTCTRL=97 LEFTALT=56 RIGHTALT=100 KEY_T=20. */
		static bool ctrl_held, alt_held;
		if (ev->keycode == 29 || ev->keycode == 97)
			ctrl_held = ev->pressed;
		else if (ev->keycode == 56 || ev->keycode == 100)
			alt_held = ev->pressed;

		if (ev->pressed && ctrl_held && alt_held && ev->keycode == 20) {
			spawn_terminal(c->wl);
			break;
		}
		if (ev->pressed && ev->keysym == 0xff1bu) { /* XKB_KEY_Escape */
			running = 0;
			break;
		}
		if (wm_key(c->wm, ev->keysym, ev->pressed, ev->alt_down)) {
			c->dirty = true;
			break;        /* WM consumed it (Alt-Tab) — don't forward */
		}
		/* Otherwise route to the focused client. The Wayland frontend takes a
		 * raw evdev keycode; CrystallineLattice takes the xkb keysym, which the
		 * driver maps to a Win32 VK (DESIGN.md §3.3). */
		if (c->wl)
			wl_frontend_keyboard_key(c->wl, ev->keycode, ev->pressed);
		if (c->xport)
			transport_keyboard_key(c->xport, c->wm->stack->focus_id,
			                       ev->keysym, ev->pressed);
		break;
	}
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

/* Context for the control socket's resolution-change callback: pointers into
 * server_run's live state so a SET_MODE can re-modeset and resize everything. */
struct mode_apply_ctx {
	int drm_fd;
	struct kms *k;
	struct dumb_fb *fb;        /* the two framebuffers */
	struct wm *wm;
	struct transport *xport;
	bool *need_modeset;
	bool *dirty;
};

/* Apply a resolution change requested over the control socket: switch the KMS
 * mode, reallocate both framebuffers, resize the virtual screen, and force a
 * modeset on the next frame. */
static bool apply_mode_cb(void *user, int w, int h, int refresh_mhz)
{
	struct mode_apply_ctx *m = user;
	int nw, nh;

	if (kms_set_mode_wh(m->k, w, h, refresh_mhz) != 0)
		return false;
	nw = m->k->mode.hdisplay;
	nh = m->k->mode.vdisplay;

	for (int i = 0; i < 2; i++) {
		dumb_fb_destroy(m->drm_fd, &m->fb[i]);
		if (dumb_fb_create(m->drm_fd, nw, nh, &m->fb[i]) != 0) {
			LOG_ERR("control: framebuffer realloc failed at %dx%d", nw, nh);
			return false;
		}
	}

	m->wm->screen_w = nw;
	m->wm->screen_h = nh;
	if (m->wm->cursor_x >= nw) m->wm->cursor_x = nw - 1;
	if (m->wm->cursor_y >= nh) m->wm->cursor_y = nh - 1;

	/* Keep the wallpaper exactly covering the new screen. */
	for (int i = 0; i < m->wm->stack->count; i++) {
		struct window *w = &m->wm->stack->windows[i];
		if (w->role == WIN_DESKTOP)
			window_set_geometry(m->wm->stack, w->id, 0, 0, nw, nh);
	}

	/* Tell the clients so Wine resizes its virtual desktop → WM_DISPLAYCHANGE →
	 * the taskbar and wallpaper refit, exactly like Windows. */
	if (m->xport)
		transport_broadcast_screen(m->xport, nw, nh);

	*m->need_modeset = true;
	*m->dirty = true;
	LOG_INFO("control: resolution changed to %dx%d", nw, nh);
	return true;
}

int server_run(int argc, char **argv)
{
	signal(SIGINT, on_sigint);
	signal(SIGTERM, on_sigint);
	signal(SIGCHLD, SIG_IGN);   /* auto-reap spawned terminals (Ctrl+Alt+T) */
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
	/* The real session boots straight into the Wine shell (below); the demo
	 * surfaces are only for headless/dev runs. $GLACIER_NO_SHELL=1 keeps the
	 * demo scene and skips launching Wine. */
	bool want_shell = !getenv("GLACIER_NO_SHELL");
	if (!want_shell)
		seed_windows(&stack, k.mode.hdisplay, k.mode.vdisplay);

	struct wm wm;
	wm_init(&wm, &stack, k.mode.hdisplay, k.mode.vdisplay);

	/* Input is best-effort. A display server with no usable mouse/keyboard
	 * should still light up the desktop, not exit and get respawned by the
	 * greeter into a black screen. If libinput can't bind (no devices, seat
	 * grabbed elsewhere, udev empty), warn and run on rather than die. */
	struct input *input = input_create(&seat, k.mode.hdisplay, k.mode.vdisplay);
	if (!input)
		LOG_WARN("input unavailable; running without pointer/keyboard");

	/* Phase 3: the CrystallineLattice transport. Native clients connect over
	 * the rendezvous socket and their windows join the same scene as the demo
	 * surfaces. Best-effort, like input: a server that can't bind the socket
	 * should still light the desktop rather than exit into a respawn loop. */
	struct transport *xport = transport_create(&stack, k.mode.hdisplay,
	                                            k.mode.vdisplay);
	if (xport && transport_listen(xport, NULL) != 0) {
		transport_destroy(xport);
		xport = NULL;
	}
	if (!xport)
		LOG_WARN("transport unavailable; no CrystallineLattice clients");

	/* Launch the Windows shell (explorer.exe /desktop=shell) as our first
	 * CrystallineLattice client — the desktop frostedglass used to provide.
	 * Best-effort, and only once the transport is listening (the shell connects
	 * back to it via $GLACIER_SOCKET). */
	if (want_shell) {
		shell_start_audio();   /* PipeWire up before Wine probes mmdevapi */
		if (xport)
			shell_launch(k.mode.hdisplay, k.mode.vdisplay,
			             transport_socket_path(xport));
		else
			LOG_WARN("shell requested but transport down; not launching Wine");
	}

	/* Phase 4: Transport B — glacier's own minimal Wayland frontend, so native
	 * Linux apps (GTK/Qt/Electron, Xwayland-bridged X11) display as peers of
	 * Wine windows, wearing the same server-drawn chrome and cursor. Like the
	 * other inputs, it is best-effort. */
	struct wl_frontend *wl = wl_frontend_create(&stack, k.mode.hdisplay,
	                                             k.mode.vdisplay);
	if (!wl)
		LOG_WARN("wayland frontend unavailable; native Linux apps won't display");

	struct loop_ctx ctx = { .wm = &wm, .wl = wl, .xport = xport, .dirty = true };
	bool master = false, need_modeset = false;
	int front = 0;

	/* Phase 5: the control socket. Control-panel applets (desk.cpl) ask the
	 * server for outputs/modes and request resolution changes over it, instead
	 * of shelling out to wlr-randr — the server is the modesetting authority. */
	struct control *ctl = control_create(&k);
	if (ctl && control_listen(ctl, NULL) != 0) {
		control_destroy(ctl);
		ctl = NULL;
	}
	if (!ctl)
		LOG_WARN("control socket unavailable; resolution change from the panel won't work");
	struct mode_apply_ctx mode_ctx = {
		.drm_fd = drm_fd, .k = &k, .fb = fb, .wm = &wm, .xport = xport,
		.need_modeset = &need_modeset, .dirty = &ctx.dirty,
	};

	/* Phase 6 hotplug: watch udev for DRM connector add/remove and re-modeset. */
	struct udev *udev = udev_new();
	struct udev_monitor *umon = udev ? udev_monitor_new_from_netlink(udev, "udev")
	                                 : NULL;
	int udev_fd = -1;
	if (umon) {
		udev_monitor_filter_add_match_subsystem_devtype(umon, "drm", NULL);
		udev_monitor_enable_receiving(umon);
		udev_fd = udev_monitor_get_fd(umon);
	} else {
		LOG_WARN("hotplug: udev monitor unavailable; connector changes won't be noticed");
	}

	/* Phase 6 idle blanking (DPMS): blank the panel after no input for a while;
	 * any input wakes it. $GLACIER_IDLE_MS overrides (0 disables). */
	const char *idle_env = getenv("GLACIER_IDLE_MS");
	int64_t idle_ms = idle_env ? atoll(idle_env) : 300000;   /* 5 min default */
	int64_t last_activity = now_ms();
	bool blanked = false;

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

		/* Idle blanking (DPMS): input resets the timer and wakes a blanked
		 * panel; sustained idle blanks it via an ACTIVE=0 atomic commit. */
		int64_t now = now_ms();
		if (ctx.activity) {
			ctx.activity = false;
			last_activity = now;
			if (blanked) {
				blanked = false;
				need_modeset = true;   /* full modeset re-enables the CRTC */
				ctx.dirty = true;
				LOG_INFO("idle: input — waking display");
			}
		}
		if (master && !blanked && idle_ms > 0 && now - last_activity > idle_ms) {
			drmModeAtomicReq *req = drmModeAtomicAlloc();
			atomic_add(req, k.crtc_id, &k.crtc_props, "ACTIVE", 0);
			if (drmModeAtomicCommit(drm_fd, req,
			                        DRM_MODE_ATOMIC_ALLOW_MODESET, NULL) == 0) {
				blanked = true;
				LOG_INFO("idle: blanking display (DPMS off)");
			}
			drmModeAtomicFree(req);
		}

		if (master && ctx.dirty && !blanked) {
			int back = front ^ 1;
			composite(&fb[back], &wm, xport ? transport_cursor(xport) : NULL);

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

		struct pollfd pfd[6] = {
			{ .fd = seat_fd(&seat), .events = POLLIN },
			{ .fd = input ? input_fd(input) : -1, .events = POLLIN },
			{ .fd = xport ? transport_fd(xport) : -1, .events = POLLIN },
			{ .fd = wl ? wl_frontend_fd(wl) : -1, .events = POLLIN },
			{ .fd = ctl ? control_fd(ctl) : -1, .events = POLLIN },
			{ .fd = udev_fd, .events = POLLIN },
		};
		if (poll(pfd, 6, 1000) < 0 && errno != EINTR)
			break;
		if (pfd[0].revents & POLLIN)
			seat_dispatch(&seat, 0);
		if (input && (pfd[1].revents & POLLIN))
			input_dispatch(input, on_input, &ctx);
		if (xport && (pfd[2].revents & POLLIN))
			if (transport_process(xport))
				ctx.dirty = true;
		if (wl && (pfd[3].revents & POLLIN))
			if (wl_frontend_dispatch(wl))
				ctx.dirty = true;
		if (ctl && (pfd[4].revents & POLLIN))
			control_process(ctl, apply_mode_cb, &mode_ctx);
		if (umon && (pfd[5].revents & POLLIN)) {
			struct udev_device *dev = udev_monitor_receive_device(umon);
			if (dev) {
				const char *hp = udev_device_get_property_value(dev, "HOTPLUG");
				if (hp && !strcmp(hp, "1")) {
					LOG_INFO("hotplug: DRM connector change — re-syncing");
					need_modeset = true;
					ctx.dirty = true;
				}
				udev_device_unref(dev);
			}
		}
		/* Keep Wayland keyboard focus in step with the WM's focused window,
		 * then push any queued pointer/keyboard events out to clients. */
		if (wl) {
			wl_frontend_keyboard_focus(wl, stack.focus_id);
			wl_frontend_flush(wl);
		}
		/* Mirror focus transitions to CrystallineLattice clients (CL_FOCUS). */
		if (xport)
			transport_update_focus(xport, stack.focus_id);

		/* Aero Snap (and future server-side resizes) changed a window's
		 * geometry: tell the owning client so it re-renders at the new size. */
		if (wm.reconfigured_id) {
			if (xport)
				transport_notify_geometry(xport, wm.reconfigured_id);
			wm.reconfigured_id = 0;
			ctx.dirty = true;
		}
	}

	if (master)
		drmDropMaster(drm_fd);
	if (umon)
		udev_monitor_unref(umon);
	if (udev)
		udev_unref(udev);
	control_destroy(ctl);
	wl_frontend_destroy(wl);
	transport_destroy(xport);
	input_destroy(input);
cleanup:
	for (int i = 0; i < have_fb; i++)
		dumb_fb_destroy(drm_fd, &fb[i]);
	kms_finish(&k);
	seat_close_device(&seat, drm_dev);
	seat_close(&seat);
	return rc;
}
