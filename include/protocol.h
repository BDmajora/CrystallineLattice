/* CrystallineLattice — the native, server-authoritative client↔server wire
 * format (Path β). This is glacier's own protocol; no Wayland vestige.
 *
 * Design (DESIGN.md §3.3), and the things that must be right in *v0* because
 * retrofitting them forces a version bump:
 *
 *   - Opaque, server-authoritative window handles. The wire uses client-scoped
 *     window ids (like an HWND the client names); the server owns the real
 *     placement, z-order and focus. Clients *request*, the shell *decides*.
 *   - Explicit window roles (NORMAL|DESKTOP|TASKBAR|TRAY|MENU|TOOLTIP) — no
 *     geometry heuristics. Mirrors enum win_role.
 *   - First-class global virtual-screen coordinates.
 *   - The render-fence fd rides in the buffer-submit (CL_COMMIT) message from
 *     the very first byte — passed as SCM_RIGHTS ancillary data, flagged by
 *     cl_buffer.has_fence. The CPU/shm path closes it; the GL path will wait.
 *   - A min-supported-version handshake, not strict equality, so glacier and
 *     Moonshine can update as separate packages without a lockstep break.
 *
 * Buffer-allocation ownership (decided now, per §3.3): the *client* allocates
 * and the server imports whatever it is handed. The first slice carries an
 * shm/memfd of XRGB8888 pixels (the wl_shm-style fallback); dma-buf import via
 * the same SCM_RIGHTS path is the next step — hence the format/modifier fields
 * are already on the wire.
 *
 * Transport: a SOCK_SEQPACKET AF_UNIX socket. SEQPACKET preserves message
 * boundaries (one sendmsg == one recvmsg), so framing is trivial and pairs
 * naturally with SCM_RIGHTS fd passing. Every message begins with uint32 type;
 * the receiver validates the datagram size against the message it names. */
#ifndef GLACIER_PROTOCOL_H
#define GLACIER_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#define CL_MAGIC        0x4C435243u  /* 'CRCL' */
#define CL_VERSION      1u           /* this build speaks */
#define CL_MIN_VERSION  1u           /* …and accepts down to here */

/* Default rendezvous: $XDG_RUNTIME_DIR/glacier-0, overridable by $GLACIER_SOCKET
 * (an absolute path). cl_default_socket_path() resolves it for both ends. */
#define CL_SOCKET_ENV   "GLACIER_SOCKET"
#define CL_SOCKET_NAME  "glacier-0"

#define CL_MAX_FDS      2            /* buffer fd [+ render-fence fd] */
#define CL_TITLE_MAX    64
#define CL_FORMAT_XRGB8888 0x34325258u /* DRM_FORMAT_XRGB8888 ('XR24') */

enum cl_msg_type {
	/* client → server */
	CL_HELLO = 1,        /* must be first; magic + version handshake */
	CL_CREATE_WINDOW,    /* name a window, request role + initial geometry */
	CL_SET_TITLE,
	CL_COMMIT,           /* submit a content buffer (fd via SCM_RIGHTS) */
	CL_DESTROY_WINDOW,
	/* server → client */
	CL_WELCOME = 128,    /* negotiated version + virtual-screen size */
	CL_CONFIGURE,        /* server-decided geometry for a window */
	CL_FOCUS,            /* focus gained/lost */
	CL_CLOSE,            /* shell asks the window to close */
	CL_INPUT,            /* pointer/keyboard routed to the focused window */
};

/* Window roles — mirror enum win_role so the mapping is a cast-with-bounds. */
enum cl_role {
	CL_ROLE_NORMAL = 0,
	CL_ROLE_DESKTOP,
	CL_ROLE_TASKBAR,
	CL_ROLE_TRAY,
	CL_ROLE_MENU,
	CL_ROLE_TOOLTIP,
	CL_ROLE_COUNT,
};

enum cl_buf_kind { CL_BUF_SHM = 0, CL_BUF_DMABUF = 1 };

enum cl_input_kind { CL_IN_MOTION = 0, CL_IN_BUTTON = 1, CL_IN_KEY = 2 };

/* Buffer descriptor inside CL_COMMIT. The fd(s) travel as ancillary data:
 * fds[0] = the buffer; if has_fence, fds[1] = an in-fence (sync_file/syncobj)
 * the server waits on before sampling the buffer. */
struct cl_buffer {
	uint32_t kind;       /* enum cl_buf_kind */
	uint32_t width, height;
	uint32_t stride;
	uint32_t format;     /* CL_FORMAT_* (DRM fourcc) */
	uint64_t modifier;   /* DRM format modifier; 0 for LINEAR/shm */
	uint32_t has_fence;  /* 1 ⇒ a render-fence fd accompanies this message */
	uint32_t _pad;
};

/* ---- messages (each begins with uint32 type) ------------------------- */

struct cl_hello {
	uint32_t type;       /* CL_HELLO */
	uint32_t magic;      /* CL_MAGIC */
	uint32_t version;    /* client's CL_VERSION */
	uint32_t min_version;/* lowest the client accepts */
};

struct cl_welcome {
	uint32_t type;       /* CL_WELCOME */
	uint32_t magic;      /* CL_MAGIC */
	uint32_t version;    /* negotiated version */
	uint32_t screen_w, screen_h;
};

struct cl_create_window {
	uint32_t type;       /* CL_CREATE_WINDOW */
	uint32_t wid;        /* client-scoped window id (nonzero) */
	uint32_t role;       /* enum cl_role */
	int32_t  x, y;       /* requested position (virtual-screen coords) */
	uint32_t w, h;       /* requested size */
};

struct cl_set_title {
	uint32_t type;       /* CL_SET_TITLE */
	uint32_t wid;
	char     title[CL_TITLE_MAX];
};

struct cl_commit {
	uint32_t type;       /* CL_COMMIT */
	uint32_t wid;
	struct cl_buffer buffer;
};

struct cl_destroy_window {
	uint32_t type;       /* CL_DESTROY_WINDOW */
	uint32_t wid;
};

struct cl_configure {
	uint32_t type;       /* CL_CONFIGURE */
	uint32_t wid;
	int32_t  x, y;
	uint32_t w, h;
};

struct cl_focus {
	uint32_t type;       /* CL_FOCUS */
	uint32_t wid;
	uint32_t focused;    /* 1 = gained, 0 = lost */
};

struct cl_close {
	uint32_t type;       /* CL_CLOSE */
	uint32_t wid;
};

struct cl_input {
	uint32_t type;       /* CL_INPUT */
	uint32_t wid;
	uint32_t kind;       /* enum cl_input_kind */
	int32_t  x, y;       /* MOTION: pointer in window-local coords */
	uint32_t code;       /* BUTTON: evdev BTN_*; KEY: xkb keysym */
	uint32_t pressed;    /* BUTTON/KEY */
};

/* ---- framed send/recv with SCM_RIGHTS (protocol.c) ------------------- */

/* Send one datagram, optionally passing nfds file descriptors. 0 on success. */
int cl_send(int fd, const void *msg, size_t len, const int *fds, int nfds);

/* Receive one datagram into buf (cap bytes). Returns the byte count (0 on
 * orderly shutdown, -1 on error). Any passed fds are written to fds[] and the
 * count to *nfds (caller owns and must close them). fds must hold CL_MAX_FDS. */
ssize_t cl_recv(int fd, void *buf, size_t cap, int *fds, int *nfds);

/* Resolve the rendezvous socket path into buf. */
void cl_default_socket_path(char *buf, size_t len);

#endif /* GLACIER_PROTOCOL_H */
