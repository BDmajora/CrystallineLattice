/* glacier — server-owned window model.
 *
 * The server holds all window state: geometry, z-order, focus, role. Clients
 * (Wine via CrystallineLattice, or the Wayland frontend) only *request*
 * changes; the shell decides. Geometry is in the single global virtual-screen
 * coordinate space spanning all outputs — the thing Wayland refuses to give
 * and Windows depends on. Pure logic, no hardware deps (unit-testable). */
#ifndef GLACIER_WINDOW_H
#define GLACIER_WINDOW_H

#include <stdbool.h>
#include <stdint.h>

/* Explicit roles — the shell places by role, never by geometry heuristic.
 * This is what retires frostedglass's is_taskbar()/is_desktop() guessing. */
enum win_role {
	WIN_NORMAL = 0,
	WIN_DESKTOP,
	WIN_TASKBAR,
	WIN_TRAY,
	WIN_MENU,
	WIN_TOOLTIP,
};

struct window {
	uint32_t id;            /* opaque server handle (HWND-like) */
	enum win_role role;
	int x, y, w, h;         /* rect in global virtual-screen coords */
	bool mapped;            /* visible + participates in hit-testing */
	uint32_t color;         /* placeholder content (XRGB8888) until a
	                         * client transport supplies a real buffer */
	char title[64];

	/* Client-supplied content (CPU/shm path). buf is NULL until a transport
	 * commits a buffer; the compositor then blits these pixels into the
	 * window's content area instead of the flat placeholder colour. The
	 * transport owns the mapping lifetime, not the window model. */
	const uint32_t *buf;    /* mmap'd XRGB8888 pixels, or NULL */
	int buf_w, buf_h;       /* buffer dimensions in pixels */
	int buf_stride;         /* row stride in bytes */
};

#define WIN_MAX 64

/* Height of the server-drawn title bar. Shared by the compositor (which draws
 * it) and the WM (which treats a press on it as a drag handle, vs. a content
 * press that passes through to the client). */
#define DECOR_TITLEBAR_H 28

/* Z-ordered list: windows[0] is bottom, windows[count-1] is top. */
struct window_stack {
	struct window windows[WIN_MAX];
	int count;
	uint32_t focus_id;      /* focused window id, or 0 for none */
	uint32_t next_id;       /* monotonic id allocator */
};

void window_stack_init(struct window_stack *s);

/* Create a mapped window on top. Returns its id, or 0 if the stack is full. */
uint32_t window_create(struct window_stack *s, enum win_role role,
                       int x, int y, int w, int h, uint32_t color,
                       const char *title);

struct window *window_by_id(struct window_stack *s, uint32_t id);
int            window_index(const struct window_stack *s, uint32_t id);
void           window_destroy(struct window_stack *s, uint32_t id);

/* Topmost mapped window whose rect contains (gx,gy); NULL if none. */
struct window *window_at(struct window_stack *s, int gx, int gy);

void window_raise(struct window_stack *s, uint32_t id);  /* to top    */
void window_lower(struct window_stack *s, uint32_t id);  /* to bottom */
void window_move(struct window_stack *s, uint32_t id, int x, int y);

/* Focus a window and raise it (server policy: focus implies raise). */
void           window_focus(struct window_stack *s, uint32_t id);
struct window *window_focused(struct window_stack *s);

/* Alt-Tab: focus+raise the next (dir>0) or previous (dir<0) NORMAL window. */
void window_focus_cycle(struct window_stack *s, int dir);

#endif /* GLACIER_WINDOW_H */
