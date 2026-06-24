/* glacier — the display server entry point.
 *
 * `glacier wm [/dev/dri/cardN]`. Phase 2 baseline: seat (seatd) + libinput +
 * a server-authoritative window manager, drawn by a CPU (pixman-style)
 * compositor with server-side decorations and a software cursor. GL
 * composition and the KMS hardware-cursor plane are the next step (Phase 1.3
 * / Phase 6 in DESIGN.md); the window model and input routing are already
 * the real server-owned ones, not placeholders. */
#ifndef GLACIER_SERVER_H
#define GLACIER_SERVER_H

int server_run(int argc, char **argv);

#endif /* GLACIER_SERVER_H */
