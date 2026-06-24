/* glacier — DRM/KMS platform layer (device, modeset target, framebuffers). */
#define _GNU_SOURCE
#include "platform.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* ---- property maps --------------------------------------------------- */
int prop_map_load(int fd, uint32_t obj_id, uint32_t obj_type, struct prop_map *out)
{
	out->props = NULL;
	out->count = 0;
	drmModeObjectProperties *props =
	        drmModeObjectGetProperties(fd, obj_id, obj_type);
	if (!props) {
		LOG_ERR("get properties of object %u: %s", obj_id, strerror(errno));
		return -1;
	}
	out->props = calloc(props->count_props, sizeof(struct prop));
	if (!out->props) {
		drmModeFreeObjectProperties(props);
		return -1;
	}
	for (uint32_t i = 0; i < props->count_props; i++) {
		drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
		if (!p)
			continue;
		struct prop *dst = &out->props[out->count++];
		snprintf(dst->name, sizeof(dst->name), "%s", p->name);
		dst->id = p->prop_id;
		dst->value = props->prop_values[i];
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	return 0;
}

void prop_map_free(struct prop_map *m)
{
	free(m->props);
	m->props = NULL;
	m->count = 0;
}

uint32_t prop_id(const struct prop_map *m, const char *name)
{
	for (int i = 0; i < m->count; i++)
		if (strcmp(m->props[i].name, name) == 0)
			return m->props[i].id;
	return 0;
}

bool prop_value(const struct prop_map *m, const char *name, uint64_t *out)
{
	for (int i = 0; i < m->count; i++)
		if (strcmp(m->props[i].name, name) == 0) {
			*out = m->props[i].value;
			return true;
		}
	return false;
}

int atomic_add(drmModeAtomicReq *req, uint32_t obj_id,
               const struct prop_map *m, const char *name, uint64_t value)
{
	uint32_t id = prop_id(m, name);
	if (!id) {
		LOG_ERR("property '%s' not found on object %u", name, obj_id);
		return -1;
	}
	if (drmModeAtomicAddProperty(req, obj_id, id, value) < 0) {
		LOG_ERR("atomic add '%s' on %u: %s", name, obj_id, strerror(errno));
		return -1;
	}
	return 0;
}

/* ---- device ---------------------------------------------------------- */
int drm_set_caps(int fd)
{
	if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
		LOG_ERR("DRM_CLIENT_CAP_ATOMIC unavailable: %s", strerror(errno));
		return -1;
	}
	if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0) {
		LOG_ERR("DRM_CLIENT_CAP_UNIVERSAL_PLANES unavailable: %s",
		        strerror(errno));
		return -1;
	}
	return 0;
}

bool drm_has_connected(int fd)
{
	drmModeRes *res = drmModeGetResources(fd);
	if (!res)
		return false;
	bool found = false;
	for (int i = 0; i < res->count_connectors && !found; i++) {
		drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
		if (c && c->connection == DRM_MODE_CONNECTED)
			found = true;
		if (c)
			drmModeFreeConnector(c);
	}
	drmModeFreeResources(res);
	return found;
}

int drm_open(const char *explicit_path)
{
	if (explicit_path) {
		int fd = open(explicit_path, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			LOG_ERR("open %s: %s", explicit_path, strerror(errno));
			return -1;
		}
		if (drm_set_caps(fd) != 0) {
			close(fd);
			return -1;
		}
		LOG_INFO("using %s", explicit_path);
		return fd;
	}

	int fallback = -1;
	for (int i = 0; i < 16; i++) {
		char path[64];
		snprintf(path, sizeof(path), "/dev/dri/card%d", i);
		int fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (drm_set_caps(fd) != 0) {
			close(fd);
			continue;
		}
		if (drm_has_connected(fd)) {
			LOG_INFO("using %s (connected)", path);
			return fd;
		}
		if (fallback < 0) {
			fallback = fd; /* keep first KMS-capable node as backup */
			LOG_INFO("fallback candidate %s (no connected connector)", path);
		} else {
			close(fd);
		}
	}
	if (fallback >= 0)
		return fallback;
	LOG_ERR("no atomic-capable DRM device found");
	return -1;
}

static const char *connector_status(int s)
{
	switch (s) {
	case DRM_MODE_CONNECTED:    return "connected";
	case DRM_MODE_DISCONNECTED: return "disconnected";
	default:                    return "unknown";
	}
}

void print_connectors(int fd)
{
	drmModeRes *res = drmModeGetResources(fd);
	if (!res) {
		LOG_ERR("drmModeGetResources: %s", strerror(errno));
		return;
	}
	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
		if (!c)
			continue;
		LOG_INFO("connector %u: %s, %d modes", c->connector_id,
		         connector_status(c->connection), c->count_modes);
		drmModeFreeConnector(c);
	}
	drmModeFreeResources(res);
}

/* ---- modeset target -------------------------------------------------- */
static int crtc_index_of(drmModeRes *res, uint32_t crtc_id)
{
	for (int i = 0; i < res->count_crtcs; i++)
		if (res->crtcs[i] == crtc_id)
			return i;
	return -1;
}

int kms_setup(int fd, struct kms *k)
{
	memset(k, 0, sizeof(*k));
	k->fd = fd;

	drmModeRes *res = drmModeGetResources(fd);
	if (!res) {
		LOG_ERR("drmModeGetResources: %s", strerror(errno));
		return -1;
	}

	drmModeConnector *conn = NULL;
	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
		if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
			conn = c;
			break;
		}
		if (c)
			drmModeFreeConnector(c);
	}
	if (!conn) {
		LOG_ERR("no connected connector with modes");
		drmModeFreeResources(res);
		return -1;
	}
	k->connector_id = conn->connector_id;

	int best = 0;
	for (int i = 0; i < conn->count_modes; i++)
		if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
			best = i;
			break;
		}
	k->mode = conn->modes[best];

	uint32_t crtc_id = 0;
	if (conn->encoder_id) {
		drmModeEncoder *e = drmModeGetEncoder(fd, conn->encoder_id);
		if (e) {
			if (e->crtc_id)
				crtc_id = e->crtc_id;
			drmModeFreeEncoder(e);
		}
	}
	for (int i = 0; i < conn->count_encoders && !crtc_id; i++) {
		drmModeEncoder *e = drmModeGetEncoder(fd, conn->encoders[i]);
		if (!e)
			continue;
		for (int c = 0; c < res->count_crtcs; c++)
			if (e->possible_crtcs & (1u << c)) {
				crtc_id = res->crtcs[c];
				break;
			}
		drmModeFreeEncoder(e);
	}
	if (!crtc_id) {
		LOG_ERR("no usable CRTC for connector %u", k->connector_id);
		drmModeFreeConnector(conn);
		drmModeFreeResources(res);
		return -1;
	}
	k->crtc_id = crtc_id;
	k->crtc_index = crtc_index_of(res, crtc_id);

	drmModePlaneRes *pres = drmModeGetPlaneResources(fd);
	if (!pres) {
		LOG_ERR("drmModeGetPlaneResources: %s", strerror(errno));
		drmModeFreeConnector(conn);
		drmModeFreeResources(res);
		return -1;
	}
	for (uint32_t i = 0; i < pres->count_planes && !k->plane_id; i++) {
		drmModePlane *pl = drmModeGetPlane(fd, pres->planes[i]);
		if (!pl)
			continue;
		if (pl->possible_crtcs & (1u << k->crtc_index)) {
			struct prop_map pm;
			uint64_t type;
			if (prop_map_load(fd, pl->plane_id, DRM_MODE_OBJECT_PLANE,
			                  &pm) == 0 &&
			    prop_value(&pm, "type", &type) &&
			    type == DRM_PLANE_TYPE_PRIMARY)
				k->plane_id = pl->plane_id;
			prop_map_free(&pm);
		}
		drmModeFreePlane(pl);
	}
	drmModeFreePlaneResources(pres);
	if (!k->plane_id) {
		LOG_ERR("no primary plane for CRTC %u", k->crtc_id);
		drmModeFreeConnector(conn);
		drmModeFreeResources(res);
		return -1;
	}

	if (prop_map_load(fd, k->connector_id, DRM_MODE_OBJECT_CONNECTOR,
	                  &k->conn_props) != 0 ||
	    prop_map_load(fd, k->crtc_id, DRM_MODE_OBJECT_CRTC,
	                  &k->crtc_props) != 0 ||
	    prop_map_load(fd, k->plane_id, DRM_MODE_OBJECT_PLANE,
	                  &k->plane_props) != 0) {
		drmModeFreeConnector(conn);
		drmModeFreeResources(res);
		kms_finish(k);
		return -1;
	}

	if (drmModeCreatePropertyBlob(fd, &k->mode, sizeof(k->mode),
	                              &k->mode_blob) != 0) {
		LOG_ERR("create mode blob: %s", strerror(errno));
		drmModeFreeConnector(conn);
		drmModeFreeResources(res);
		kms_finish(k);
		return -1;
	}

	LOG_INFO("connector=%u crtc=%u(idx %d) primary_plane=%u mode=%s %dx%d@%d",
	         k->connector_id, k->crtc_id, k->crtc_index, k->plane_id,
	         k->mode.name, k->mode.hdisplay, k->mode.vdisplay,
	         k->mode.vrefresh);

	drmModeFreeConnector(conn);
	drmModeFreeResources(res);
	return 0;
}

void kms_finish(struct kms *k)
{
	if (k->mode_blob)
		drmModeDestroyPropertyBlob(k->fd, k->mode_blob);
	k->mode_blob = 0;
	prop_map_free(&k->conn_props);
	prop_map_free(&k->crtc_props);
	prop_map_free(&k->plane_props);
}

int kms_atomic_modeset(drmModeAtomicReq *req, struct kms *k)
{
	int r = 0;
	r |= atomic_add(req, k->connector_id, &k->conn_props, "CRTC_ID", k->crtc_id);
	r |= atomic_add(req, k->crtc_id, &k->crtc_props, "MODE_ID", k->mode_blob);
	r |= atomic_add(req, k->crtc_id, &k->crtc_props, "ACTIVE", 1);
	return r;
}

int kms_atomic_plane(drmModeAtomicReq *req, struct kms *k, uint32_t fb_id)
{
	int r = 0;
	r |= atomic_add(req, k->plane_id, &k->plane_props, "FB_ID", fb_id);
	r |= atomic_add(req, k->plane_id, &k->plane_props, "CRTC_ID", k->crtc_id);
	r |= atomic_add(req, k->plane_id, &k->plane_props, "SRC_X", 0);
	r |= atomic_add(req, k->plane_id, &k->plane_props, "SRC_Y", 0);
	r |= atomic_add(req, k->plane_id, &k->plane_props, "SRC_W",
	                (uint64_t)k->mode.hdisplay << 16);
	r |= atomic_add(req, k->plane_id, &k->plane_props, "SRC_H",
	                (uint64_t)k->mode.vdisplay << 16);
	r |= atomic_add(req, k->plane_id, &k->plane_props, "CRTC_X", 0);
	r |= atomic_add(req, k->plane_id, &k->plane_props, "CRTC_Y", 0);
	r |= atomic_add(req, k->plane_id, &k->plane_props, "CRTC_W", k->mode.hdisplay);
	r |= atomic_add(req, k->plane_id, &k->plane_props, "CRTC_H", k->mode.vdisplay);
	return r;
}

/* ---- dumb framebuffer ------------------------------------------------ */
int dumb_fb_create(int fd, int w, int h, struct dumb_fb *fb)
{
	memset(fb, 0, sizeof(*fb));
	fb->map = MAP_FAILED;
	fb->w = w;
	fb->h = h;

	struct drm_mode_create_dumb create = {
	        .width = (uint32_t)w, .height = (uint32_t)h, .bpp = 32 };
	if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
		LOG_ERR("create dumb buffer: %s", strerror(errno));
		return -1;
	}
	fb->handle = create.handle;
	fb->stride = create.pitch;
	fb->size = create.size;

	uint32_t handles[4] = { fb->handle };
	uint32_t strides[4] = { fb->stride };
	uint32_t offsets[4] = { 0 };
	if (drmModeAddFB2(fd, w, h, DRM_FORMAT_XRGB8888, handles, strides,
	                  offsets, &fb->fb_id, 0) != 0) {
		LOG_ERR("AddFB2: %s", strerror(errno));
		goto err;
	}

	struct drm_mode_map_dumb map = { .handle = fb->handle };
	if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
		LOG_ERR("map dumb buffer: %s", strerror(errno));
		goto err;
	}
	fb->map = mmap(NULL, fb->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
	               map.offset);
	if (fb->map == MAP_FAILED) {
		LOG_ERR("mmap dumb buffer: %s", strerror(errno));
		goto err;
	}
	memset(fb->map, 0, fb->size);
	return 0;

err:
	dumb_fb_destroy(fd, fb);
	return -1;
}

void dumb_fb_destroy(int fd, struct dumb_fb *fb)
{
	if (fb->map && fb->map != MAP_FAILED)
		munmap(fb->map, fb->size);
	if (fb->fb_id)
		drmModeRmFB(fd, fb->fb_id);
	if (fb->handle) {
		struct drm_mode_destroy_dumb d = { .handle = fb->handle };
		drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
	}
	memset(fb, 0, sizeof(*fb));
}
