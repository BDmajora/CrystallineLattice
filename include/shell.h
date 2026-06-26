/* glacier — Wine desktop-shell launcher.
 *
 * Brings up the Windows shell (explorer.exe /desktop=shell) as a
 * CrystallineLattice client of this server, so booting glacier lands you on the
 * Wine desktop — the role frostedglass used to fill from a wlroots compositor.
 * Ported from frostedglass src/fg_wine.c, retargeted from the Wayland driver to
 * winedrm.drv (Path β): the child gets $GLACIER_SOCKET instead of
 * $WAYLAND_DISPLAY and the Wine graphics driver is forced to "drm". */
#ifndef GLACIER_SHELL_H
#define GLACIER_SHELL_H

/* Fork the Wine shell pointed at cl_socket_path, sized to width×height. Runs
 * one-time prefix/sound/registry setup first. Best-effort: logs and returns
 * (a failed shell must not take the display server down). The caller owns the
 * decision of whether to launch (e.g. honour $GLACIER_NO_SHELL). */
void shell_launch(int width, int height, const char *cl_socket_path);

#endif /* GLACIER_SHELL_H */
