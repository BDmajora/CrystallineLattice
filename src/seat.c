/* glacier — seat session via libseat/seatd (see seat.h). */
#define _GNU_SOURCE
#include "seat.h"
#include "log.h"

#include <errno.h>
#include <libseat.h>
#include <string.h>

static void on_enable(struct libseat *seat, void *data)
{
	(void)seat;
	((struct seat *)data)->active = true;
	LOG_INFO("seat enabled");
}

static void on_disable(struct libseat *seat, void *data)
{
	struct seat *s = data;
	s->active = false;
	LOG_INFO("seat disabled (VT away)");
	libseat_disable_seat(seat); /* acknowledge so seatd can hand off */
}

int seat_open(struct seat *s)
{
	static struct libseat_seat_listener listener = {
		.enable_seat = on_enable,
		.disable_seat = on_disable,
	};
	memset(s, 0, sizeof(*s));
	s->handle = libseat_open_seat(&listener, s);
	if (!s->handle) {
		LOG_ERR("libseat_open_seat: %s (is seatd running, user in 'seat'?)",
		        strerror(errno));
		return -1;
	}
	/* Pump until the seat is active so device opens succeed. */
	while (!s->active)
		if (libseat_dispatch(s->handle, -1) < 0) {
			LOG_ERR("libseat_dispatch: %s", strerror(errno));
			return -1;
		}
	return 0;
}

int seat_fd(struct seat *s)
{
	return libseat_get_fd(s->handle);
}

int seat_dispatch(struct seat *s, int timeout_ms)
{
	return libseat_dispatch(s->handle, timeout_ms);
}

bool seat_active(struct seat *s)
{
	return s->active;
}

int seat_open_device(struct seat *s, const char *path, int *out_fd)
{
	int fd = -1;
	int id = libseat_open_device(s->handle, path, &fd);
	if (id < 0 || fd < 0) {
		LOG_ERR("seat open %s: %s", path, strerror(errno));
		return -1;
	}
	*out_fd = fd;
	return id;
}

void seat_close_device(struct seat *s, int dev_id)
{
	libseat_close_device(s->handle, dev_id);
}

void seat_close(struct seat *s)
{
	if (s->handle)
		libseat_close_seat(s->handle);
	s->handle = NULL;
}
