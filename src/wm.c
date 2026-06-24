/* glacier — window-manager policy (see wm.h). */
#include "wm.h"

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

void wm_pointer_motion(struct wm *wm, double dx, double dy)
{
	wm->cursor_x = clampi(wm->cursor_x + (int)dx, 0, wm->screen_w - 1);
	wm->cursor_y = clampi(wm->cursor_y + (int)dy, 0, wm->screen_h - 1);

	if (wm->moving) {
		/* Keep the grabbed point under the cursor. */
		window_move(wm->stack, wm->move_id,
		            wm->cursor_x - wm->grab_dx,
		            wm->cursor_y - wm->grab_dy);
	}
}

void wm_pointer_button(struct wm *wm, uint32_t button, bool pressed)
{
	if (button != WM_BTN_LEFT)
		return;

	if (pressed) {
		struct window *w = window_at(wm->stack, wm->cursor_x, wm->cursor_y);
		if (!w)
			return;
		/* Capture before window_focus(), which reorders the stack and
		 * would leave `w` dangling at a now-different slot. */
		uint32_t id = w->id;
		enum win_role role = w->role;
		int wx = w->x, wy = w->y;

		window_focus(wm->stack, id);              /* focus + raise */
		/* The desktop/wallpaper is not draggable. */
		if (role == WIN_DESKTOP)
			return;
		wm->moving = true;
		wm->move_id = id;
		wm->grab_dx = wm->cursor_x - wx;
		wm->grab_dy = wm->cursor_y - wy;
	} else {
		wm->moving = false;
		wm->move_id = 0;
	}
}

bool wm_key(struct wm *wm, uint32_t keysym, bool pressed, bool alt_down)
{
	if (pressed && alt_down && keysym == WM_KEY_TAB) {
		window_focus_cycle(wm->stack, +1);
		return true;
	}
	return false;
}
