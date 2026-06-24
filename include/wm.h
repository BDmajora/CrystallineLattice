/* glacier — window-manager policy (server-authoritative).
 *
 * Routes input over the window stack and owns the cursor position in global
 * virtual-screen coordinates. The server, not any client, decides focus,
 * z-order and geometry — the X-server+WM / win32k+DWM model from the spec.
 * Pure logic, no hardware deps (unit-testable). */
#ifndef GLACIER_WM_H
#define GLACIER_WM_H

#include "window.h"

/* Canonical evdev/xkb values, redefined locally so this stays dependency-free
 * and testable without <linux/input-event-codes.h> or xkbcommon. */
#define WM_BTN_LEFT 0x110u   /* BTN_LEFT  */
#define WM_KEY_TAB  0xff09u  /* XKB_KEY_Tab */

struct wm {
	struct window_stack *stack;
	int screen_w, screen_h;   /* virtual-screen bounds (cursor clamp) */
	int cursor_x, cursor_y;

	bool moving;              /* interactive move-drag in progress */
	uint32_t move_id;        /* window being dragged */
	int grab_dx, grab_dy;    /* cursor offset within the grabbed window */
};

void wm_init(struct wm *wm, struct window_stack *stack, int screen_w, int screen_h);

/* Relative pointer motion (libinput dx/dy). Clamps the cursor to the
 * virtual screen; drags the grabbed window when a move is active. */
void wm_pointer_motion(struct wm *wm, double dx, double dy);

/* Pointer button. Left-press over a window focuses+raises it and starts an
 * interactive move; release ends the move. */
void wm_pointer_button(struct wm *wm, uint32_t button, bool pressed);

/* Keyboard. Returns true if the WM consumed the key (e.g. Alt-Tab), meaning
 * the caller must NOT also forward it to the focused client. */
bool wm_key(struct wm *wm, uint32_t keysym, bool pressed, bool alt_down);

#endif /* GLACIER_WM_H */
