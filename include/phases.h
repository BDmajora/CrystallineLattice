/* glacier Phase 0 — per-sub-phase entry points, dispatched from main.c. */
#ifndef GLACIER_PHASES_H
#define GLACIER_PHASES_H

/* Each runs one P0.x sub-phase. argv[1] (if present) is an explicit
 * /dev/dri/cardN; otherwise the device is auto-selected. */
int p0_1_enum_run(int argc, char **argv);       /* P0.1 enumeration      */
int p0_2_firstlight_run(int argc, char **argv); /* P0.2 solid color      */
int p0_3_flip_run(int argc, char **argv);       /* P0.3 page-flip loop   */
int p0_4_gl_run(int argc, char **argv);         /* P0.4 GBM/EGL/GLES     */
int p0_5_seatd_run(int argc, char **argv);      /* P0.5 seatd + VT       */
int p0_6_hotplug_run(int argc, char **argv);    /* P0.6 hotplug          */

#endif /* GLACIER_PHASES_H */
