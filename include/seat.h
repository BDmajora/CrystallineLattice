/* glacier — seat session via libseat/seatd.
 *
 * Acquires the seat (DRM master + input device access) without logind, and
 * tracks activation so VT switches are handled. The server and libinput both
 * open their device fds through here. */
#ifndef GLACIER_SEAT_H
#define GLACIER_SEAT_H

#include <stdbool.h>

struct libseat;

struct seat {
	struct libseat *handle;
	bool active;
};

/* Open the seat and pump events until it is active. Returns 0 on success. */
int  seat_open(struct seat *s);
int  seat_fd(struct seat *s);                      /* fd to poll() */
int  seat_dispatch(struct seat *s, int timeout_ms);
bool seat_active(struct seat *s);

/* Open/close a device (DRM node, input device) through the seat. Returns a
 * device id (>= 0) and writes the opened fd via out_fd; -1 on failure. */
int  seat_open_device(struct seat *s, const char *path, int *out_fd);
void seat_close_device(struct seat *s, int dev_id);

void seat_close(struct seat *s);

#endif /* GLACIER_SEAT_H */
