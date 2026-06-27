/* glacier — server-owned window model (see window.h). */
#include "window.h"

#include <stdio.h>
#include <string.h>

void window_stack_init(struct window_stack *s)
{
	memset(s, 0, sizeof(*s));
	s->next_id = 1;
}

uint32_t window_create(struct window_stack *s, enum win_role role,
                       int x, int y, int w, int h, uint32_t color,
                       const char *title)
{
	if (s->count >= WIN_MAX)
		return 0;
	struct window *win = &s->windows[s->count++];
	memset(win, 0, sizeof(*win));
	win->id = s->next_id++;
	win->role = role;
	win->x = x; win->y = y; win->w = w; win->h = h;
	win->color = color;
	win->mapped = true;
	if (title)
		snprintf(win->title, sizeof(win->title), "%s", title);
	return win->id;
}

int window_index(const struct window_stack *s, uint32_t id)
{
	for (int i = 0; i < s->count; i++)
		if (s->windows[i].id == id)
			return i;
	return -1;
}

struct window *window_by_id(struct window_stack *s, uint32_t id)
{
	int i = window_index(s, id);
	return i < 0 ? NULL : &s->windows[i];
}

void window_destroy(struct window_stack *s, uint32_t id)
{
	int idx = window_index(s, id);
	if (idx < 0)
		return;
	for (int i = idx; i < s->count - 1; i++)
		s->windows[i] = s->windows[i + 1];
	s->count--;
	if (s->focus_id == id)
		s->focus_id = s->count ? s->windows[s->count - 1].id : 0;
}

struct window *window_at(struct window_stack *s, int gx, int gy)
{
	/* Topmost first: walk the stack from the top down. */
	for (int i = s->count - 1; i >= 0; i--) {
		struct window *w = &s->windows[i];
		if (!w->mapped)
			continue;
		if (gx >= w->x && gx < w->x + w->w &&
		    gy >= w->y && gy < w->y + w->h)
			return w;
	}
	return NULL;
}

/* Move the entry at idx to the top (count-1), preserving the others' order. */
static void move_to_top(struct window_stack *s, int idx)
{
	if (idx < 0 || idx >= s->count - 1)
		return;
	struct window tmp = s->windows[idx];
	for (int i = idx; i < s->count - 1; i++)
		s->windows[i] = s->windows[i + 1];
	s->windows[s->count - 1] = tmp;
}

static void move_to_bottom(struct window_stack *s, int idx)
{
	if (idx <= 0 || idx >= s->count)
		return;
	struct window tmp = s->windows[idx];
	for (int i = idx; i > 0; i--)
		s->windows[i] = s->windows[i - 1];
	s->windows[0] = tmp;
}

void window_raise(struct window_stack *s, uint32_t id)
{
	move_to_top(s, window_index(s, id));
}

void window_lower(struct window_stack *s, uint32_t id)
{
	move_to_bottom(s, window_index(s, id));
}

void window_move(struct window_stack *s, uint32_t id, int x, int y)
{
	struct window *w = window_by_id(s, id);
	if (w) {
		w->x = x;
		w->y = y;
	}
}

void window_set_geometry(struct window_stack *s, uint32_t id,
                         int x, int y, int w, int h)
{
	struct window *win = window_by_id(s, id);
	if (!win)
		return;
	win->x = x;
	win->y = y;
	if (w > 0)
		win->w = w;
	if (h > 0)
		win->h = h;
}

void window_focus(struct window_stack *s, uint32_t id)
{
	struct window *w = window_by_id(s, id);
	if (!w)
		return;
	s->focus_id = id;
	if (w->role != WIN_DESKTOP)   /* the desktop/wallpaper stays at the bottom */
		window_raise(s, id);
}

struct window *window_focused(struct window_stack *s)
{
	return s->focus_id ? window_by_id(s, s->focus_id) : NULL;
}

void window_focus_cycle(struct window_stack *s, int dir)
{
	if (s->count == 0)
		return;
	int start = window_index(s, s->focus_id); /* -1 if none focused */
	int step = dir >= 0 ? 1 : -1;
	for (int n = 1; n <= s->count; n++) {
		int i = ((start + step * n) % s->count + s->count) % s->count;
		if (s->windows[i].role == WIN_NORMAL && s->windows[i].mapped) {
			window_focus(s, s->windows[i].id);
			return;
		}
	}
}
