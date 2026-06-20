/* P0.5 — seatd + VT switching (drop root, coexist).
 * Opens the DRM device through libseat (seatd/logind backend), repaints a
 * solid color whenever the seat is active, releases master and stops on
 * seat-disable (VT-away), and repaints on re-enable (VT-return). Run as a
 * normal user in the "seat" group; switch VTs to verify. */
#define _GNU_SOURCE
#include "common.h"
#include "phases.h"

#include <errno.h>
#include <libseat.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;
static void on_sigint(int s) { (void)s; running = 0; }

struct state {
	struct libseat *seat;
	int active;       /* seat enabled */
	int painted;      /* current frame committed */
	int fd;           /* DRM fd from seat */
	int dev_id;       /* libseat device id */
	struct kms k;
	int have_kms;
	struct dumb_fb fb;
	int have_fb;
};

static void on_enable(struct libseat *seat, void *data)
{
	(void)seat;
	((struct state *)data)->active = 1;
	LOG_INFO("seat enabled");
}
static void on_disable(struct libseat *seat, void *data)
{
	struct state *st = data;
	st->active = 0;
	LOG_INFO("seat disabled");
	libseat_disable_seat(seat); /* acknowledge */
}

static int paint(struct state *st)
{
	if (drmSetMaster(st->fd) != 0)
		LOG_WARN("drmSetMaster: %s", strerror(errno));
	if (!st->have_kms) {
		if (kms_setup(st->fd, &st->k) != 0)
			return -1;
		st->have_kms = 1;
	}
	if (!st->have_fb) {
		if (dumb_fb_create(st->fd, st->k.mode.hdisplay,
		                   st->k.mode.vdisplay, &st->fb) != 0)
			return -1;
		for (int y = 0; y < st->fb.h; y++) {
			uint32_t *row =
			        (uint32_t *)(st->fb.map + (size_t)y * st->fb.stride);
			for (int x = 0; x < st->fb.w; x++)
				row[x] = 0x00207040u; /* a green */
		}
		st->have_fb = 1;
	}
	drmModeAtomicReq *req = drmModeAtomicAlloc();
	kms_atomic_modeset(req, &st->k);
	kms_atomic_plane(req, &st->k, st->fb.fb_id);
	int r = drmModeAtomicCommit(st->fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET,
	                            NULL);
	drmModeAtomicFree(req);
	if (r != 0) {
		LOG_ERR("commit: %s", strerror(errno));
		return -1;
	}
	st->painted = 1;
	return 0;
}

int p0_5_seatd_run(int argc, char **argv)
{
	signal(SIGINT, on_sigint);
	const char *path = argc > 1 ? argv[1] : "/dev/dri/card0";

	struct state st = { .fd = -1 };
	struct libseat_seat_listener listener = {
	        .enable_seat = on_enable, .disable_seat = on_disable };

	st.seat = libseat_open_seat(&listener, &st);
	if (!st.seat) {
		LOG_ERR("libseat_open_seat: %s (is seatd running?)",
		        strerror(errno));
		return 1;
	}
	int sfd = libseat_get_fd(st.seat);

	/* Pump until the seat becomes active so device open succeeds. */
	while (!st.active && running) {
		if (libseat_dispatch(st.seat, -1) < 0) {
			LOG_ERR("libseat_dispatch: %s", strerror(errno));
			return 1;
		}
	}

	st.dev_id = libseat_open_device(st.seat, path, &st.fd);
	if (st.dev_id < 0 || st.fd < 0) {
		LOG_ERR("libseat_open_device %s: %s", path, strerror(errno));
		return 1;
	}
	if (drm_set_caps(st.fd) != 0)
		return 1;
	LOG_INFO("opened %s via seat (fd %d)", path, st.fd);

	while (running) {
		if (st.active && !st.painted) {
			if (paint(&st) != 0)
				break;
		} else if (!st.active && st.painted) {
			drmDropMaster(st.fd);
			st.painted = 0;
		}
		struct pollfd pfd = { .fd = sfd, .events = POLLIN };
		if (poll(&pfd, 1, 1000) > 0)
			libseat_dispatch(st.seat, 0);
	}

	if (st.have_fb)
		dumb_fb_destroy(st.fd, &st.fb);
	if (st.have_kms)
		kms_finish(&st.k);
	if (st.fd >= 0)
		libseat_close_device(st.seat, st.dev_id);
	libseat_close_seat(st.seat);
	return 0;
}
