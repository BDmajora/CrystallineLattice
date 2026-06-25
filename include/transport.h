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

struct window_stack;
struct transport;

/* Create the transport over `stack` (which it mutates) for a virtual screen of
 * sw×sh. No socket is bound yet — call transport_listen(). NULL on failure. */
struct transport *transport_create(struct window_stack *stack, int sw, int sh);

/* Bind + listen on `path` (NULL ⇒ cl_default_socket_path) and start accepting.
 * Returns 0 on success. */
int transport_listen(struct transport *t, const char *path);

/* The single fd the server should poll() for readability. */
int transport_fd(struct transport *t);

/* Service all ready events (new connections, client messages, disconnects).
 * Returns true if the scene changed and the server should recomposite. */
bool transport_process(struct transport *t);

/* Register an already-connected client fd. Used by accept(), and directly by
 * the headless self-test to inject a socketpair end. Returns 0 on success. */
int transport_add_client(struct transport *t, int fd);

void transport_destroy(struct transport *t);

#endif /* GLACIER_TRANSPORT_H */
