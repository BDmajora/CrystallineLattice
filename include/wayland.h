/* glacier — Transport B: the Wayland compatibility frontend (Phase 4).
 *
 * glacier implements its *own* minimal Wayland server (libwayland-server +
 * a frozen core-protocol subset), NOT wlroots and NOT Weston. It hosts native
 * Linux toolkits (GTK/Qt/Electron) and, via Xwayland, legacy X11 apps. Each
 * Wayland toplevel is reparented as a server-owned glacier window with role
 * NORMAL, feeding the *same* window stack and CPU compositor as the
 * CrystallineLattice transport — so foreign apps wear the same server-drawn
 * Windows title bar and the same server-owned cursor as Wine apps.
 *
 * What it speaks: wl_compositor, wl_surface, wl_shm (CPU buffers), xdg_wm_base
 * (xdg_surface/xdg_toplevel), wl_seat (pointer+keyboard), wl_output, and
 * xdg-decoration set to *force* server-side decorations (CSD forbidden). The
 * cursor is the KMS/CPU cursor the server owns, so wl_pointer.set_cursor is
 * deliberately ignored. */
#ifndef GLACIER_WAYLAND_H
#define GLACIER_WAYLAND_H

#include <stdbool.h>
#include <stdint.h>

struct window_stack;
struct wl_frontend;

/* Create the frontend over `stack` for a virtual screen of sw×sh, and bind a
 * Wayland socket ($XDG_RUNTIME_DIR/wayland-N). NULL on failure. */
struct wl_frontend *wl_frontend_create(struct window_stack *stack, int sw, int sh);

/* The event-loop fd the server should poll() for readability. */
int  wl_frontend_fd(struct wl_frontend *fe);

/* Service ready client events. Returns true if the scene changed (a surface
 * was mapped, committed, resized or destroyed) and the server should repaint. */
bool wl_frontend_dispatch(struct wl_frontend *fe);

/* Flush queued events out to clients — call once per main-loop iteration after
 * routing input, so pointer/keyboard events reach the apps promptly. */
void wl_frontend_flush(struct wl_frontend *fe);

/* The bound socket name (e.g. "wayland-1"), for $WAYLAND_DISPLAY on spawn. */
const char *wl_frontend_socket(struct wl_frontend *fe);

/* Input routing from glacier's libinput/WM into the focused Wayland surface.
 * Coordinates are global virtual-screen pixels; keycode is a raw evdev code. */
void wl_frontend_pointer_motion(struct wl_frontend *fe, int gx, int gy);
void wl_frontend_pointer_button(struct wl_frontend *fe, uint32_t button, bool pressed);
void wl_frontend_keyboard_key(struct wl_frontend *fe, uint32_t keycode, bool pressed);

/* Tell the frontend which glacier window now holds keyboard focus. If it is a
 * Wayland surface, keyboard enter/leave is delivered; otherwise a no-op.
 * Idempotent — only acts on an actual change. */
void wl_frontend_keyboard_focus(struct wl_frontend *fe, uint32_t window_id);

void wl_frontend_destroy(struct wl_frontend *fe);

#endif /* GLACIER_WAYLAND_H */
