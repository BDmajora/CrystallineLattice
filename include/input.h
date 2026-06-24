/* glacier — input via libinput, device fds opened through seatd.
 *
 * Normalizes raw libinput into relative pointer motion, buttons, and
 * xkb-translated keys (with an Alt-held snapshot for WM shortcuts). The
 * bridge from the metal to the server-authoritative WM. */
#ifndef GLACIER_INPUT_H
#define GLACIER_INPUT_H

#include <stdbool.h>
#include <stdint.h>

struct seat;
struct input;

enum input_kind { INPUT_MOTION, INPUT_BUTTON, INPUT_KEY };

struct input_event {
	enum input_kind kind;
	double dx, dy;       /* MOTION: relative motion */
	uint32_t button;     /* BUTTON: evdev BTN_* code */
	uint32_t keysym;     /* KEY: xkb keysym */
	bool pressed;        /* BUTTON / KEY: true on press */
	bool alt_down;       /* KEY: Alt modifier held */
};

typedef void (*input_handler)(const struct input_event *ev, void *user);

struct input *input_create(struct seat *seat);  /* NULL on failure */
int   input_fd(struct input *in);
int   input_dispatch(struct input *in, input_handler cb, void *user);
void  input_destroy(struct input *in);

#endif /* GLACIER_INPUT_H */
