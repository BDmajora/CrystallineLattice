/* P0.3 — Double-buffer + page-flip loop (vsync).
 * Two dumb buffers, atomic page flips with PAGE_FLIP_EVENT, flip-event
 * loop over poll(), animates a CPU-drawn moving rectangle. This is the
 * frame loop everything else hangs off. Run on a bare VT as root. */
#define _GNU_SOURCE
#include "common.h"
#include "phases.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;
static void on_sigint(int s) { (void)s; running = 0; }

static void draw_frame(struct dumb_fb *fb, int rx)
{
	const int rw = 240, rh = 200;
	int ry = fb->h / 2 - rh / 2;
	for (int y = 0; y < fb->h; y++) {
		uint32_t *row = (uint32_t *)(fb->map + (size_t)y * fb->stride);
		int in_y = (y >= ry && y < ry + rh);
		for (int x = 0; x < fb->w; x++) {
			int in = in_y && x >= rx && x < rx + rw;
			row[x] = in ? 0x00ff7a18u : 0x00101820u;
		}
	}
}

struct flip_ctx { int pending; };
static void on_flip(int fd, unsigned seq, unsigned sec, unsigned usec,
                    unsigned crtc, void *data)
{
	(void)fd; (void)seq; (void)sec; (void)usec; (void)crtc;
	((struct flip_ctx *)data)->pending = 0;
}

int p0_3_flip_run(int argc, char **argv)
{
	signal(SIGINT, on_sigint);

	int fd = drm_open(argc > 1 ? argv[1] : NULL);
	if (fd < 0)
		return 1;
	if (drmSetMaster(fd) != 0)
		LOG_WARN("drmSetMaster: %s (need a bare VT as root)", strerror(errno));

	struct kms k;
	if (kms_setup(fd, &k) != 0)
		return 1;

	struct dumb_fb fb[2];
	for (int i = 0; i < 2; i++)
		if (dumb_fb_create(fd, k.mode.hdisplay, k.mode.vdisplay, &fb[i]) != 0)
			return 1;

	/* initial blocking modeset on buffer 0 */
	draw_frame(&fb[0], 0);
	drmModeAtomicReq *req = drmModeAtomicAlloc();
	kms_atomic_modeset(req, &k);
	kms_atomic_plane(req, &k, fb[0].fb_id);
	if (drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL) != 0) {
		LOG_ERR("initial modeset: %s", strerror(errno));
		drmModeAtomicFree(req);
		return 1;
	}
	drmModeAtomicFree(req);

	drmEventContext ev = { .version = 3, .page_flip_handler2 = on_flip };
	struct flip_ctx flip = { 0 };
	int front = 0, rx = 0, dir = 6;
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	while (running) {
		int back = !front;
		rx += dir;
		if (rx <= 0 || rx + 240 >= fb[back].w)
			dir = -dir;
		draw_frame(&fb[back], rx);

		req = drmModeAtomicAlloc();
		atomic_add(req, k.plane_id, &k.plane_props, "FB_ID", fb[back].fb_id);
		flip.pending = 1;
		if (drmModeAtomicCommit(fd, req,
		                        DRM_MODE_ATOMIC_NONBLOCK |
		                        DRM_MODE_PAGE_FLIP_EVENT,
		                        &flip) != 0) {
			LOG_ERR("flip commit: %s", strerror(errno));
			drmModeAtomicFree(req);
			break;
		}
		drmModeAtomicFree(req);

		while (flip.pending && running) {
			struct pollfd pfd = { .fd = fd, .events = POLLIN };
			if (poll(&pfd, 1, 1000) <= 0)
				continue;
			drmHandleEvent(fd, &ev);
		}
		front = back;

		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec - t0.tv_sec >= 15)
			running = 0; /* auto-exit after 15s */
	}

	for (int i = 0; i < 2; i++)
		dumb_fb_destroy(fd, &fb[i]);
	kms_finish(&k);
	drmDropMaster(fd);
	close(fd);
	return 0;
}
