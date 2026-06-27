/* glacier — window-manager policy (see wm.h). */
#include "wm.h"

/* Distance (px) from a screen edge at which a drag-release triggers Aero Snap. */
#define SNAP_EDGE 16

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

void wm_init(struct wm *wm, struct window_stack *stack, int screen_w, int screen_h)
{
	wm->stack = stack;
	wm->screen_w = screen_w;
	wm->screen_h = screen_h;
	wm->cursor_x = screen_w / 2;
	wm->cursor_y = screen_h / 2;
	wm->moving = false;
	wm->move_id = 0;
	wm->grab_dx = wm->grab_dy = 0;
}

/* After the cursor moves, drag the grabbed window so its grabbed point stays
 * under the pointer. Shared by relative and absolute motion. */
static void drag_grabbed(struct wm *wm)
{
	if (wm->moving)
		window_move(wm->stack, wm->move_id,
		            wm->cursor_x - wm->grab_dx,
		            wm->cursor_y - wm->grab_dy);
}

void wm_pointer_motion(struct wm *wm, double dx, double dy)
{
	wm->cursor_x = clampi(wm->cursor_x + (int)dx, 0, wm->screen_w - 1);
	wm->cursor_y = clampi(wm->cursor_y + (int)dy, 0, wm->screen_h - 1);
	drag_grabbed(wm);
}

void wm_pointer_motion_abs(struct wm *wm, double ax, double ay)
{
	wm->cursor_x = clampi((int)ax, 0, wm->screen_w - 1);
	wm->cursor_y = clampi((int)ay, 0, wm->screen_h - 1);
	drag_grabbed(wm);
}

/* Work area = the virtual screen minus a docked taskbar, so a maximized window
 * doesn't bury the taskbar. Handles a taskbar docked to any single edge; the
 * bottom dock (Windows' default, per StuckRects2) is the common case. */
static void wm_work_area(const struct wm *wm, int *ox, int *oy, int *ow, int *oh)
{
	const struct window_stack *s = wm->stack;
	int x = 0, y = 0, w = wm->screen_w, h = wm->screen_h;

	for (int i = 0; i < s->count; i++) {
		const struct window *t = &s->windows[i];
		if (t->role != WIN_TASKBAR || !t->mapped)
			continue;
		bool full_w = t->w >= wm->screen_w * 3 / 4;
		bool full_h = t->h >= wm->screen_h * 3 / 4;
		if (full_w && t->y <= 0) {              /* docked top */
			int b = t->y + t->h;
			if (b > y) { h -= b - y; y = b; }
		} else if (full_w) {                    /* docked bottom (default) */
			if (t->y < y + h) h = t->y - y;
		} else if (full_h && t->x <= 0) {       /* docked left */
			int r = t->x + t->w;
			if (r > x) { w -= r - x; x = r; }
		} else if (full_h) {                    /* docked right */
			if (t->x < x + w) w = t->x - x;
		}
		break;
	}
	if (w < 1) w = wm->screen_w;
	if (h < 1) h = wm->screen_h;
	*ox = x; *oy = y; *ow = w; *oh = h;
}

/* Aero Snap on drag-release: if the cursor reached a screen edge, snap the
 * window — top → maximize, left/right → that half of the work area. The
 * pre-snap rect is saved so a later drag restores the floating size. */
static void wm_snap_on_release(struct wm *wm, uint32_t id)
{
	struct window *w = window_by_id(wm->stack, id);
	int ax, ay, aw, ah, nx, ny, nw, nh;

	if (!w || !window_has_ssd(w))
		return;

	wm_work_area(wm, &ax, &ay, &aw, &ah);

	if (wm->cursor_y <= SNAP_EDGE) {                       /* top → maximize */
		nx = ax; ny = ay; nw = aw; nh = ah;
	} else if (wm->cursor_x <= SNAP_EDGE) {               /* left half */
		nx = ax; ny = ay; nw = aw / 2; nh = ah;
	} else if (wm->cursor_x >= wm->screen_w - SNAP_EDGE) { /* right half */
		nw = aw / 2; nx = ax + aw - nw; ny = ay; nh = ah;
	} else {
		return;                                           /* not at an edge */
	}

	if (!w->snapped) {
		w->saved_x = w->x; w->saved_y = w->y;
		w->saved_w = w->w; w->saved_h = w->h;
	}
	w->snapped = true;
	window_set_geometry(wm->stack, id, nx, ny, nw, nh);
	wm->reconfigured_id = id;
}

void wm_pointer_button(struct wm *wm, uint32_t button, bool pressed)
{
	struct window *w;
	uint32_t id;
	int wx, wy;

	if (button != WM_BTN_LEFT)
		return;

	if (!pressed) {
		/* Release ends a drag and is the moment we evaluate Aero Snap. */
		if (wm->moving) {
			id = wm->move_id;
			wm->moving = false;
			wm->move_id = 0;
			wm_snap_on_release(wm, id);
		}
		return;
	}

	w = window_at(wm->stack, wm->cursor_x, wm->cursor_y);
	if (!w)
		return;
	id = w->id;

	window_focus(wm->stack, id);              /* focus + raise (reorders stack) */

	/* The server title bar is a drag handle only for windows glacier decorates
	 * (native Wayland apps). Wine windows draw and drag their own Win32 title
	 * bar — that press passes through to the client, which moves the window via
	 * CL_SET_GEOMETRY. Shell surfaces never drag either. */
	w = window_by_id(wm->stack, id);          /* re-fetch after the reorder */
	if (!w || !window_has_ssd(w))
		return;
	if (wm->cursor_y - w->y >= DECOR_TITLEBAR_H)
		return;                              /* content press → passes through */

	wx = w->x; wy = w->y;

	/* Dragging a snapped window restores its floating size under the cursor,
	 * the way Windows un-maximizes when you drag the title bar. */
	if (w->snapped) {
		int nw = w->saved_w > 0 ? w->saved_w : w->w;
		int nh = w->saved_h > 0 ? w->saved_h : w->h;
		wx = wm->cursor_x - nw / 2;
		wy = wm->cursor_y - DECOR_TITLEBAR_H / 2;
		if (wx < 0) wx = 0;
		if (wy < 0) wy = 0;
		window_set_geometry(wm->stack, id, wx, wy, nw, nh);
		w->snapped = false;
		wm->reconfigured_id = id;
	}

	wm->moving = true;
	wm->move_id = id;
	wm->grab_dx = wm->cursor_x - wx;
	wm->grab_dy = wm->cursor_y - wy;
}

bool wm_key(struct wm *wm, uint32_t keysym, bool pressed, bool alt_down)
{
	if (pressed && alt_down && keysym == WM_KEY_TAB) {
		window_focus_cycle(wm->stack, +1);
		return true;
	}
	return false;
}
