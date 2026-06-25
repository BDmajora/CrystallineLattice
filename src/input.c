/* glacier — libinput wiring through seatd, xkb keysym translation (see input.h). */
#define _GNU_SOURCE
#include "input.h"
#include "seat.h"
#include "log.h"

#include <libinput.h>
#include <libudev.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

#define MAX_DEVS 64

struct input {
	struct seat *seat;
	struct udev *udev;
	struct libinput *li;
	struct xkb_context *xkb;
	struct xkb_keymap *keymap;
	struct xkb_state *state;
	struct { int fd; int devid; } devs[MAX_DEVS];
	int ndev;
	int width, height;   /* virtual-screen bounds for absolute motion */
};

/* libinput opens/closes device fds through these — we route to seatd so the
 * process never needs root or direct device permissions. */
static int open_restricted(const char *path, int flags, void *user)
{
	(void)flags;
	struct input *in = user;
	int fd = -1;
	int id = seat_open_device(in->seat, path, &fd);
	if (id < 0)
		return -1;
	if (in->ndev < MAX_DEVS) {
		in->devs[in->ndev].fd = fd;
		in->devs[in->ndev].devid = id;
		in->ndev++;
	}
	return fd;
}

static void close_restricted(int fd, void *user)
{
	struct input *in = user;
	for (int i = 0; i < in->ndev; i++) {
		if (in->devs[i].fd == fd) {
			seat_close_device(in->seat, in->devs[i].devid);
			in->devs[i] = in->devs[in->ndev - 1];
			in->ndev--;
			return;
		}
	}
}

static const struct libinput_interface iface = {
	.open_restricted = open_restricted,
	.close_restricted = close_restricted,
};

struct input *input_create(struct seat *seat, int width, int height)
{
	struct input *in = calloc(1, sizeof(*in));
	if (!in)
		return NULL;
	in->seat = seat;
	in->width = width;
	in->height = height;

	in->udev = udev_new();
	if (!in->udev) {
		LOG_ERR("udev_new failed");
		goto err;
	}
	in->li = libinput_udev_create_context(&iface, in, in->udev);
	if (!in->li) {
		LOG_ERR("libinput_udev_create_context failed");
		goto err;
	}
	if (libinput_udev_assign_seat(in->li, "seat0") != 0) {
		LOG_ERR("libinput_udev_assign_seat(seat0) failed");
		goto err;
	}

	in->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (in->xkb) {
		in->keymap = xkb_keymap_new_from_names(in->xkb, NULL,
		                                       XKB_KEYMAP_COMPILE_NO_FLAGS);
		if (in->keymap)
			in->state = xkb_state_new(in->keymap);
	}
	if (!in->state)
		LOG_WARN("xkb keymap unavailable; keysyms will be raw");

	return in;

err:
	input_destroy(in);
	return NULL;
}

int input_fd(struct input *in)
{
	return libinput_get_fd(in->li);
}

static void emit_pointer_motion(struct libinput_event *e, input_handler cb, void *u)
{
	struct libinput_event_pointer *p = libinput_event_get_pointer_event(e);
	struct input_event ev = {
		.kind = INPUT_MOTION,
		.dx = libinput_event_pointer_get_dx(p),
		.dy = libinput_event_pointer_get_dy(p),
	};
	cb(&ev, u);
}

/* Absolute motion: the device reports a position, not a delta. libinput
 * transforms it into our virtual-screen pixel space. Without this, mice on
 * QEMU/VM tablets (which only emit ABSOLUTE events) leave the cursor frozen. */
static void emit_pointer_motion_abs(struct input *in, struct libinput_event *e,
                                    input_handler cb, void *u)
{
	struct libinput_event_pointer *p = libinput_event_get_pointer_event(e);
	struct input_event ev = {
		.kind = INPUT_MOTION_ABS,
		.ax = libinput_event_pointer_get_absolute_x_transformed(p, in->width),
		.ay = libinput_event_pointer_get_absolute_y_transformed(p, in->height),
	};
	cb(&ev, u);
}

static void emit_pointer_button(struct libinput_event *e, input_handler cb, void *u)
{
	struct libinput_event_pointer *p = libinput_event_get_pointer_event(e);
	struct input_event ev = {
		.kind = INPUT_BUTTON,
		.button = libinput_event_pointer_get_button(p),
		.pressed = libinput_event_pointer_get_button_state(p) ==
		           LIBINPUT_BUTTON_STATE_PRESSED,
	};
	cb(&ev, u);
}

static void emit_key(struct input *in, struct libinput_event *e,
                     input_handler cb, void *u)
{
	struct libinput_event_keyboard *k = libinput_event_get_keyboard_event(e);
	uint32_t code = libinput_event_keyboard_get_key(k);
	bool pressed = libinput_event_keyboard_get_key_state(k) ==
	               LIBINPUT_KEY_STATE_PRESSED;

	struct input_event ev = { .kind = INPUT_KEY, .pressed = pressed,
	                          .keycode = code };
	if (in->state) {
		xkb_keycode_t xkc = code + 8; /* evdev → xkb keycode offset */
		ev.keysym = xkb_state_key_get_one_sym(in->state, xkc);
		xkb_state_update_key(in->state, xkc,
		                     pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
		ev.alt_down = xkb_state_mod_name_is_active(
		        in->state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0;
		ev.ctrl_down = xkb_state_mod_name_is_active(
		        in->state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0;
	} else {
		ev.keysym = code; /* raw fallback */
	}
	cb(&ev, u);
}

int input_dispatch(struct input *in, input_handler cb, void *user)
{
	if (libinput_dispatch(in->li) != 0)
		return -1;
	struct libinput_event *e;
	while ((e = libinput_get_event(in->li))) {
		switch (libinput_event_get_type(e)) {
		case LIBINPUT_EVENT_POINTER_MOTION:
			emit_pointer_motion(e, cb, user);
			break;
		case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
			emit_pointer_motion_abs(in, e, cb, user);
			break;
		case LIBINPUT_EVENT_POINTER_BUTTON:
			emit_pointer_button(e, cb, user);
			break;
		case LIBINPUT_EVENT_KEYBOARD_KEY:
			emit_key(in, e, cb, user);
			break;
		default:
			break;
		}
		libinput_event_destroy(e);
	}
	return 0;
}

void input_destroy(struct input *in)
{
	if (!in)
		return;
	if (in->state)
		xkb_state_unref(in->state);
	if (in->keymap)
		xkb_keymap_unref(in->keymap);
	if (in->xkb)
		xkb_context_unref(in->xkb);
	if (in->li)
		libinput_unref(in->li);
	if (in->udev)
		udev_unref(in->udev);
	free(in);
}
