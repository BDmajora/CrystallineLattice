/* glacier — DRM/KMS platform: device open, modeset target, dumb framebuffers. */
#ifndef GLACIER_PLATFORM_H
#define GLACIER_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "log.h"

#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* ---- atomic property maps -------------------------------------------- */
struct prop {
	char name[DRM_PROP_NAME_LEN];
	uint32_t id;
	uint64_t value;
};
struct prop_map {
	struct prop *props;
	int count;
};
int      prop_map_load(int fd, uint32_t obj_id, uint32_t obj_type, struct prop_map *out);
void     prop_map_free(struct prop_map *m);
uint32_t prop_id(const struct prop_map *m, const char *name);
bool     prop_value(const struct prop_map *m, const char *name, uint64_t *out);
int      atomic_add(drmModeAtomicReq *req, uint32_t obj_id,
                    const struct prop_map *m, const char *name, uint64_t value);

/* ---- device ---------------------------------------------------------- */
/* Opens a DRM primary node, sets ATOMIC + UNIVERSAL_PLANES caps. When
 * explicit_path is NULL, scans /dev/dri/cardN and prefers a node with a
 * connected connector. Returns fd >= 0 or -1. */
int drm_open(const char *explicit_path);
int drm_set_caps(int fd);
/* Brief connector-state listing (used by P0.1 and P0.6). */
void print_connectors(int fd);

/* ---- modeset target -------------------------------------------------- */
struct kms {
	int fd;
	uint32_t connector_id, crtc_id, plane_id;
	int crtc_index;
	drmModeModeInfo mode;
	uint32_t mode_blob;
	struct prop_map conn_props, crtc_props, plane_props;
};
/* Picks first connected connector + a CRTC + its primary plane, loads
 * property maps, and creates the mode blob. Returns 0 on success. */
int  kms_setup(int fd, struct kms *k);
void kms_finish(struct kms *k);
/* Add connector/CRTC modeset state to an atomic request. */
int  kms_atomic_modeset(drmModeAtomicReq *req, struct kms *k);
/* Add full primary-plane state (FB_ID + SRC/CRTC rects) to a request. */
int  kms_atomic_plane(drmModeAtomicReq *req, struct kms *k, uint32_t fb_id);

/* ---- dumb (CPU) framebuffer ------------------------------------------ */
struct dumb_fb {
	uint32_t fb_id, handle, stride;
	uint64_t size;
	uint8_t *map;
	int w, h;
};
int  dumb_fb_create(int fd, int w, int h, struct dumb_fb *fb);
void dumb_fb_destroy(int fd, struct dumb_fb *fb);

#endif /* GLACIER_PLATFORM_H */
