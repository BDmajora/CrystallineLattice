/* P0.2 — First light (dumb buffer, atomic, solid color).
 * Becomes DRM master, allocates a dumb buffer, fills it, and binds
 * connector -> CRTC -> primary plane in one atomic commit (TEST_ONLY
 * first). Run on a bare VT as root. No GBM/GL. */
#define _GNU_SOURCE
#include "platform.h"
#include "diagnostics.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void fill_solid(struct dumb_fb *fb, uint32_t xrgb)
{
	for (int y = 0; y < fb->h; y++) {
		uint32_t *row = (uint32_t *)(fb->map + (size_t)y * fb->stride);
		for (int x = 0; x < fb->w; x++)
			row[x] = xrgb;
	}
}

int diag_firstlight(int argc, char **argv)
{
	int fd = drm_open(argc > 1 ? argv[1] : NULL);
	if (fd < 0)
		return 1;
	if (drmSetMaster(fd) != 0)
		LOG_WARN("drmSetMaster: %s (need a bare VT as root)", strerror(errno));

	struct kms k;
	if (kms_setup(fd, &k) != 0)
		return 1;

	struct dumb_fb fb;
	if (dumb_fb_create(fd, k.mode.hdisplay, k.mode.vdisplay, &fb) != 0)
		return 1;
	fill_solid(&fb, 0x00104080); /* a steel blue */

	drmModeAtomicReq *req = drmModeAtomicAlloc();
	if (kms_atomic_modeset(req, &k) != 0 ||
	    kms_atomic_plane(req, &k, fb.fb_id) != 0) {
		LOG_ERR("failed to build atomic request");
		drmModeAtomicFree(req);
		return 1;
	}

	if (drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_TEST_ONLY |
	                                 DRM_MODE_ATOMIC_ALLOW_MODESET,
	                        NULL) != 0) {
		LOG_ERR("atomic TEST_ONLY failed: %s", strerror(errno));
		drmModeAtomicFree(req);
		return 1;
	}
	if (drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL) != 0) {
		LOG_ERR("atomic commit failed: %s", strerror(errno));
		drmModeAtomicFree(req);
		return 1;
	}
	drmModeAtomicFree(req);

	LOG_INFO("solid color up for 5s");
	sleep(5);

	dumb_fb_destroy(fd, &fb);
	kms_finish(&k);
	drmDropMaster(fd);
	close(fd);
	return 0;
}
