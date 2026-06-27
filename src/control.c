/* glacier — control socket (see control.h). */
#define _GNU_SOURCE
#include "control.h"
#include "platform.h"
#include "log.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct control {
	struct kms *k;
	int listen_fd;
	char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
};

void glctl_default_socket_path(char *buf, uint32_t len)
{
	const char *override = getenv(GLCTL_SOCKET_ENV);
	const char *rt;
	if (override && override[0]) {
		snprintf(buf, len, "%s", override);
		return;
	}
	rt = getenv("XDG_RUNTIME_DIR");
	if (rt && rt[0])
		snprintf(buf, len, "%s/%s", rt, GLCTL_SOCKET_NAME);
	else
		snprintf(buf, len, "/tmp/%s", GLCTL_SOCKET_NAME);
}

/* A short, stable, xrandr-ish connector name from its DRM type. */
static void connector_name(const drmModeConnector *c, char *buf, size_t len)
{
	const char *t;
	switch (c->connector_type) {
	case DRM_MODE_CONNECTOR_eDP:         t = "eDP"; break;
	case DRM_MODE_CONNECTOR_LVDS:        t = "LVDS"; break;
	case DRM_MODE_CONNECTOR_DisplayPort: t = "DP"; break;
	case DRM_MODE_CONNECTOR_HDMIA:       t = "HDMI-A"; break;
	case DRM_MODE_CONNECTOR_HDMIB:       t = "HDMI-B"; break;
	case DRM_MODE_CONNECTOR_VGA:         t = "VGA"; break;
	case DRM_MODE_CONNECTOR_DVII:
	case DRM_MODE_CONNECTOR_DVID:
	case DRM_MODE_CONNECTOR_DVIA:        t = "DVI"; break;
	case DRM_MODE_CONNECTOR_VIRTUAL:     t = "Virtual"; break;
	default:                             t = "Display"; break;
	}
	snprintf(buf, len, "%s-%u", t, c->connector_type_id);
}

static uint32_t mode_refresh_mhz(const drmModeModeInfo *m)
{
	if (m->htotal && m->vtotal)
		return (uint32_t)((uint64_t)m->clock * 1000000u /
		                  ((uint32_t)m->htotal * m->vtotal));
	return m->vrefresh * 1000u;
}

struct control *control_create(struct kms *k)
{
	struct control *c = calloc(1, sizeof(*c));
	if (!c)
		return NULL;
	c->k = k;
	c->listen_fd = -1;
	return c;
}

int control_listen(struct control *c, const char *path)
{
	char resolved[sizeof(c->path)];
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int fd;

	if (path)
		snprintf(resolved, sizeof(resolved), "%s", path);
	else
		glctl_default_socket_path(resolved, sizeof(resolved));

	fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		LOG_ERR("control: socket: %s", strerror(errno));
		return -1;
	}
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", resolved);
	unlink(resolved);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
	    listen(fd, 4) != 0) {
		LOG_ERR("control: bind/listen %s: %s", resolved, strerror(errno));
		close(fd);
		return -1;
	}
	c->listen_fd = fd;
	snprintf(c->path, sizeof(c->path), "%s", resolved);
	LOG_INFO("control: listening on %s", resolved);
	return 0;
}

int control_fd(struct control *c)
{
	return c->listen_fd;
}

static void handle_get_outputs(struct control *c, int cfd)
{
	struct glctl_outputs out;
	drmModeConnector *conn;
	struct glctl_output *o;

	memset(&out, 0, sizeof(out));
	out.type = GLCTL_OUTPUTS;
	out.magic = GLCTL_MAGIC;

	conn = drmModeGetConnector(c->k->fd, c->k->connector_id);
	if (conn) {
		o = &out.outputs[out.num_outputs++];
		connector_name(conn, o->name, sizeof(o->name));
		for (int i = 0; i < conn->count_modes &&
		                o->num_modes < GLCTL_MAX_MODES; i++) {
			const drmModeModeInfo *m = &conn->modes[i];
			struct glctl_mode *gm;
			if (m->flags & DRM_MODE_FLAG_INTERLACE)
				continue;
			gm = &o->modes[o->num_modes++];
			gm->w = m->hdisplay;
			gm->h = m->vdisplay;
			gm->refresh_mhz = mode_refresh_mhz(m);
			if (m->type & DRM_MODE_TYPE_PREFERRED)
				gm->flags |= GLCTL_MODE_PREFERRED;
			if (m->hdisplay == c->k->mode.hdisplay &&
			    m->vdisplay == c->k->mode.vdisplay &&
			    m->vrefresh == c->k->mode.vrefresh)
				gm->flags |= GLCTL_MODE_CURRENT;
		}
		drmModeFreeConnector(conn);
	}

	send(cfd, &out, sizeof(out), MSG_NOSIGNAL);
}

void control_process(struct control *c, control_apply_mode_fn apply, void *user)
{
	for (;;) {
		int cfd = accept4(c->listen_fd, NULL, NULL,
		                  SOCK_CLOEXEC | SOCK_NONBLOCK);
		struct glctl_request req;
		ssize_t n;
		if (cfd < 0)
			return;   /* EAGAIN: drained */

		n = recv(cfd, &req, sizeof(req), 0);
		if (n >= (ssize_t)offsetof(struct glctl_request, output) &&
		    req.magic == GLCTL_MAGIC) {
			if (req.type == GLCTL_GET_OUTPUTS) {
				handle_get_outputs(c, cfd);
			} else if (req.type == GLCTL_SET_MODE) {
				struct glctl_result r = {
					.type = GLCTL_RESULT, .magic = GLCTL_MAGIC,
					.ok = apply && apply(user, (int)req.w, (int)req.h,
					                     (int)req.refresh_mhz),
				};
				LOG_INFO("control: set mode %ux%u@%umHz → %s",
				         req.w, req.h, req.refresh_mhz, r.ok ? "ok" : "fail");
				send(cfd, &r, sizeof(r), MSG_NOSIGNAL);
			}
		}
		close(cfd);
	}
}

void control_destroy(struct control *c)
{
	if (!c)
		return;
	if (c->listen_fd >= 0)
		close(c->listen_fd);
	if (c->path[0])
		unlink(c->path);
	free(c);
}
