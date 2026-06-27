/* glacier — unit test for the server-owned window model + WM policy.
 * Pure logic, no hardware: builds a stack, exercises hit-testing, focus,
 * z-order, interactive move, and Alt-Tab. Run from `glacier`'s build. */
#include "window.h"
#include "wm.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
	struct window_stack s;
	window_stack_init(&s);

	uint32_t desk = window_create(&s, WIN_DESKTOP, 0, 0, 1920, 1080,
	                              0x202020, "desktop");
	uint32_t a = window_create(&s, WIN_NORMAL, 100, 100, 400, 300,
	                           0xc04040, "A");
	uint32_t b = window_create(&s, WIN_NORMAL, 300, 200, 400, 300,
	                           0x40c040, "B");
	assert(s.count == 3);

	/* Hit-testing returns the topmost mapped window. */
	assert(window_at(&s, 350, 250)->id == b);   /* overlap → B (newest) */
	assert(window_at(&s, 120, 120)->id == a);   /* only A */
	assert(window_at(&s, 10, 10)->id == desk);  /* only desktop */

	/* Focus raises (focus implies raise). */
	window_focus(&s, a);
	assert(s.focus_id == a);
	assert(window_at(&s, 350, 250)->id == a);   /* A now above B */

	/* Interactive move via the WM. */
	struct wm wm;
	wm_init(&wm, &s, 1920, 1080);

	/* A content press focuses but does NOT start a drag — it passes through to
	 * the client (Phase 4: native apps stay interactive). B is at (300,200);
	 * (650,450) is well below its title bar. */
	wm.cursor_x = 650; wm.cursor_y = 450;
	wm_pointer_button(&wm, WM_BTN_LEFT, true);
	assert(s.focus_id == b && !wm.moving);
	wm_pointer_button(&wm, WM_BTN_LEFT, false);

	/* A title-bar press (within DECOR_TITLEBAR_H of the top) starts a move. */
	wm.cursor_x = 650; wm.cursor_y = 205;        /* over B's title bar */
	wm_pointer_button(&wm, WM_BTN_LEFT, true);
	assert(s.focus_id == b && wm.moving);
	int bx = window_by_id(&s, b)->x, by = window_by_id(&s, b)->y;
	wm_pointer_motion(&wm, 25, -10);
	assert(window_by_id(&s, b)->x == bx + 25);
	assert(window_by_id(&s, b)->y == by - 10);
	wm_pointer_button(&wm, WM_BTN_LEFT, false);
	assert(!wm.moving);

	/* Alt-Tab cycles between NORMAL windows, skipping the desktop. */
	uint32_t before = s.focus_id;
	assert(wm_key(&wm, WM_KEY_TAB, true, true));  /* consumed */
	assert(s.focus_id != before);
	assert(window_focused(&s)->role == WIN_NORMAL);

	/* Clicking the desktop focuses it but never raises it or starts a move. */
	wm.cursor_x = 5; wm.cursor_y = 5;
	wm_pointer_button(&wm, WM_BTN_LEFT, true);
	assert(s.focus_id == desk && !wm.moving);
	assert(window_at(&s, 350, 250)->role == WIN_NORMAL); /* desktop still bottom */
	wm_pointer_button(&wm, WM_BTN_LEFT, false);

	/* Destroy moves focus to the new top. */
	window_destroy(&s, desk);
	assert(s.count == 2 && window_by_id(&s, desk) == NULL);

	/* ---- Aero Snap (Phase 5) ----------------------------------------- */
	{
		struct window_stack ss;
		struct wm w2;
		struct window *aw;
		window_stack_init(&ss);
		/* A bottom-docked taskbar (full width, 40px) → work area is screen
		 * minus those 40px. */
		window_create(&ss, WIN_TASKBAR, 0, 1040, 1920, 40, 0, "taskbar");
		uint32_t app = window_create(&ss, WIN_NORMAL, 500, 400, 300, 200, 0, "app");
		wm_init(&w2, &ss, 1920, 1080);

		/* Drag the title bar and release at the LEFT edge → snap to the left
		 * half of the work area; the floating rect is remembered and the
		 * owning client is flagged for a CONFIGURE. */
		w2.cursor_x = 550; w2.cursor_y = 405;        /* over app's title bar */
		wm_pointer_button(&w2, WM_BTN_LEFT, true);
		assert(w2.moving);
		w2.cursor_x = 2; w2.cursor_y = 540;          /* release at left edge */
		wm_pointer_button(&w2, WM_BTN_LEFT, false);
		aw = window_by_id(&ss, app);
		assert(aw->snapped);
		assert(aw->x == 0 && aw->y == 0 && aw->w == 960 && aw->h == 1040);
		assert(aw->saved_w == 300 && aw->saved_h == 200);
		assert(w2.reconfigured_id == app);

		/* Dragging the snapped window's title bar restores the floating size. */
		w2.reconfigured_id = 0;
		w2.cursor_x = 400; w2.cursor_y = 5;
		wm_pointer_button(&w2, WM_BTN_LEFT, true);
		aw = window_by_id(&ss, app);
		assert(!aw->snapped && aw->w == 300 && aw->h == 200);
		assert(w2.reconfigured_id == app);
		w2.cursor_x = 600; w2.cursor_y = 540;        /* release mid-screen → no snap */
		wm_pointer_button(&w2, WM_BTN_LEFT, false);
		aw = window_by_id(&ss, app);
		assert(!aw->snapped && aw->w == 300);

		/* Drag to the TOP edge → maximize to the work area (not over taskbar). */
		w2.cursor_x = aw->x + 20; w2.cursor_y = aw->y + 5;
		wm_pointer_button(&w2, WM_BTN_LEFT, true);
		w2.cursor_x = 960; w2.cursor_y = 2;          /* release at top edge */
		wm_pointer_button(&w2, WM_BTN_LEFT, false);
		aw = window_by_id(&ss, app);
		assert(aw->snapped && aw->x == 0 && aw->y == 0);
		assert(aw->w == 1920 && aw->h == 1040);      /* full screen minus taskbar */
	}

	printf("wm selftest: OK (incl. Aero Snap)\n");
	return 0;
}
