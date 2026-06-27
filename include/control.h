/* glacier — control socket (Phase 5).
 *
 * "Everything is a path": the shell's control-panel applets talk to the display
 * server over one small request/response socket instead of shelling out to
 * wlr-randr (DESIGN.md §1, §3.4). Today it serves display enumeration and live
 * resolution changes — the server is the modesetting authority and does the
 * atomic commit itself. desk.cpl's unix lib speaks this protocol.
 *
 * Transport: a SOCK_SEQPACKET AF_UNIX socket, one request datagram → one
 * response datagram, connection-per-call. Rendezvous: $GLACIER_CTL_SOCKET, else
 * $XDG_RUNTIME_DIR/glacier-ctl. */
#ifndef GLACIER_CONTROL_H
#define GLACIER_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

struct kms;
struct control;

/* ---- wire format (duplicated, kept in sync, in desk.cpl's unix lib) ---- */

#define GLCTL_MAGIC        0x4743544Cu  /* 'GCTL' */
#define GLCTL_SOCKET_ENV   "GLACIER_CTL_SOCKET"
#define GLCTL_SOCKET_NAME  "glacier-ctl"
#define GLCTL_MAX_OUTPUTS  4
#define GLCTL_MAX_MODES    64

enum glctl_type {
	GLCTL_GET_OUTPUTS = 1,   /* req  → GLCTL_OUTPUTS */
	GLCTL_SET_MODE,          /* req  → GLCTL_RESULT  */
	GLCTL_OUTPUTS = 128,     /* resp */
	GLCTL_RESULT,            /* resp */
};

#define GLCTL_MODE_PREFERRED 0x1u
#define GLCTL_MODE_CURRENT   0x2u

struct glctl_mode {
	uint32_t w, h;
	uint32_t refresh_mhz;    /* millihertz, 60000 = 60.000 Hz */
	uint32_t flags;          /* GLCTL_MODE_* */
};

struct glctl_output {
	char     name[32];       /* connector name, e.g. "eDP-1" / "Virtual-1" */
	int32_t  x, y;           /* layout position */
	uint32_t num_modes;
	struct glctl_mode modes[GLCTL_MAX_MODES];
};

struct glctl_request {
	uint32_t type;           /* GLCTL_GET_OUTPUTS | GLCTL_SET_MODE */
	uint32_t magic;          /* GLCTL_MAGIC */
	char     output[32];     /* SET_MODE: target connector ("" = primary) */
	uint32_t w, h;           /* SET_MODE */
	uint32_t refresh_mhz;    /* SET_MODE: 0 = don't care */
};

struct glctl_outputs {
	uint32_t type;           /* GLCTL_OUTPUTS */
	uint32_t magic;
	uint32_t num_outputs;
	struct glctl_output outputs[GLCTL_MAX_OUTPUTS];
};

struct glctl_result {
	uint32_t type;           /* GLCTL_RESULT */
	uint32_t magic;
	uint32_t ok;             /* 1 = success */
};

/* ---- server side ----------------------------------------------------- */

/* Apply a resolution change: the server picks the matching connector mode, does
 * the atomic modeset, reallocates framebuffers and updates the virtual-screen
 * size. Returns true on success. */
typedef bool (*control_apply_mode_fn)(void *user, int w, int h, int refresh_mhz);

/* Create over an existing KMS target (read for enumeration + current mode).
 * No socket bound yet — call control_listen(). NULL on failure. */
struct control *control_create(struct kms *k);

/* Bind + listen on `path` (NULL ⇒ default rendezvous). 0 on success. */
int  control_listen(struct control *c, const char *path);

/* The fd the server should poll() for readability (the listen socket). */
int  control_fd(struct control *c);

/* Service all pending control requests; `apply` does SET_MODE work. */
void control_process(struct control *c, control_apply_mode_fn apply, void *user);

void control_destroy(struct control *c);

/* Resolve the rendezvous path into buf (shared with desk.cpl). */
void glctl_default_socket_path(char *buf, uint32_t len);

#endif /* GLACIER_CONTROL_H */
