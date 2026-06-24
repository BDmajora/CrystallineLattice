/* glacier — Phase-0 platform diagnostics, dispatched from main.c.
 *
 * Each runs one read-only or modeset check against the DRM device to prove
 * the metal cooperates. argv[1] (if present) is an explicit /dev/dri/cardN;
 * otherwise the device is auto-selected. These are bring-up tools, not the
 * server — see server.h / `glacier wm` for the real display server. */
#ifndef GLACIER_DIAGNOSTICS_H
#define GLACIER_DIAGNOSTICS_H

int diag_enumerate(int argc, char **argv);  /* resource enumeration (no master)   */
int diag_firstlight(int argc, char **argv); /* dumb-buffer atomic modeset, solid  */
int diag_pageflip(int argc, char **argv);   /* double-buffer page-flip loop (vsync) */
int diag_gl(int argc, char **argv);         /* GBM/EGL/GLES triangle, scanned out  */
int diag_seat(int argc, char **argv);       /* seatd session + VT switching        */
int diag_hotplug(int argc, char **argv);    /* udev DRM hotplug monitor (no master) */

#endif /* GLACIER_DIAGNOSTICS_H */
