/* CrystallineLattice — server-side transport (Path β, Phase 3).
 *
 * Accepts native clients over a SOCK_SEQPACKET unix socket, runs the v0
 * handshake, and turns their requests into server-owned windows in the shared
 * window_stack. Clients only *request* role/geometry/buffers; the shell owns
 * placement, z-order and focus. Buffer fds arrive via SCM_RIGHTS; the shm/CPU
 * path mmaps them for the compositor (dma-buf import is the next step).
 *
 * The transport multiplexes the listen socket and every client fd onto a
 * single epoll fd, so the server's poll loop adds exactly one descriptor. */
#ifndef GLACIER_TRANSPORT_H
#define GLACIER_TRANSPORT_H

#include <stdbool.h>
#include <stdint.h>

struct window_stack;
struct transport;

/* The cursor shape the focused client wants (CL_SET_CURSOR). The server owns
 * the cursor position and draws this shape, falling back to its built-in arrow
 * when `custom` is false. ARGB8888, straight alpha, capped to 64x64. */
#define CL_CURSOR_MAX 64
struct cl_cursor {
	bool     custom;        /* false ⇒ draw the built-in arrow */
	bool     hidden;        /* true  ⇒ draw nothing */
	int      w, h;
	int      hotspot_x, hotspot_y;
	uint32_t argb[CL_CURSOR_MAX * CL_CURSOR_MAX];
};

/* Create the transport over `stack` (which it mutates) for a virtual screen of
 * sw×sh. No socket is bound yet — call transport_listen(). NULL on failure. */
struct transport *transport_create(struct window_stack *stack, int sw, int sh);

/* Bind + listen on `path` (NULL ⇒ cl_default_socket_path) and start accepting.
 * Returns 0 on success. */
int transport_listen(struct transport *t, const char *path);

/* The single fd the server should poll() for readability. */
int transport_fd(struct transport *t);

/* The bound rendezvous socket path (valid after transport_listen), so the
 * server can hand it to a spawned client as $GLACIER_SOCKET. */
const char *transport_socket_path(struct transport *t);

/* Update the virtual-screen size advertised to new clients (after a resolution
 * change). Existing clients keep their windows; new connections see the size. */
void transport_set_screen(struct transport *t, int w, int h);

/* Tell every connected client the virtual screen resized (CL_SCREEN), so each
 * resizes its Wine virtual desktop and the shell refits. Also updates the size
 * advertised to new clients. */
void transport_broadcast_screen(struct transport *t, int w, int h);

/* The cursor shape last set by a client; the compositor draws it. */
const struct cl_cursor *transport_cursor(struct transport *t);

/* Service all ready events (new connections, client messages, disconnects).
 * Returns true if the scene changed and the server should recomposite. */
bool transport_process(struct transport *t);

/* Register an already-connected client fd. Used by accept(), and directly by
 * the headless self-test to inject a socketpair end. Returns 0 on success. */
int transport_add_client(struct transport *t, int fd);

/* ---- input routing to CrystallineLattice clients (Phase 3, M2) -------- *
 * The server owns the cursor and focus; these forward the relevant input to
 * the owning client as CL_INPUT, in window-local content coordinates. They are
 * no-ops when the target window isn't a CL client (e.g. a Wayland-frontend
 * window or the desktop), so the server can call them unconditionally. */

/* Pointer at the given virtual-screen position → the topmost CL client window
 * under the cursor (content area only; a press/move over the server title bar
 * belongs to the WM, not the client). */
void transport_pointer_motion(struct transport *t, int cursor_x, int cursor_y);
void transport_pointer_button(struct transport *t, uint32_t button, bool pressed,
                              int cursor_x, int cursor_y);

/* Key (xkb keysym) → the CL client that owns the focused window. */
void transport_keyboard_key(struct transport *t, uint32_t focus_id,
                            uint32_t keysym, bool pressed);

/* Emit CL_FOCUS transitions when the server's focused window changes; call once
 * per loop with the current stack focus id. */
void transport_update_focus(struct transport *t, uint32_t focus_id);

/* Re-send a window's server-decided geometry to its owning client (CL_CONFIGURE)
 * after the WM changed it server-side (Aero Snap, control-panel resize). No-op
 * if the id isn't a CrystallineLattice client window. */
void transport_notify_geometry(struct transport *t, uint32_t server_id);

void transport_destroy(struct transport *t);

#endif /* GLACIER_TRANSPORT_H */
