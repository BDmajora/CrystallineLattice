# glacier — the CrystallineLattice display server

**glacier** is the display-server component of **CrystallineLattice**: a
from-scratch, *server-authoritative* DRM/KMS display server purpose-built as the
shell for an open-source Windows environment (YetiOS). It talks to DRM/KMS,
GBM/EGL and libinput directly — **no wlroots, no Wayland compositor underneath**.
The full design and phased roadmap live in [DESIGN.md](DESIGN.md).

## Status

- **Phase 0 — platform bring-up: done.** DRM enumeration, atomic modeset, the
  page-flip loop, GBM/EGL/GLES, seatd + VT switching, and hotplug — exposed as
  the `glacier <diag>` commands below.
- **Phase 2 — input + server-authoritative WM: in progress.** The server-owned
  window model, WM policy (focus, z-order, interactive move, Alt-Tab) and a CPU
  (pixman-style) compositor with server-side decorations and a software cursor
  are landed as `glacier wm`. GL composition and the KMS hardware-cursor plane
  are the next step; the window model and input routing are already the real
  server-owned ones, not placeholders.

## Layout

```
main.c                 subcommand dispatch
include/ + src/
  log.*                leveled logging
  platform.*           DRM/KMS: device open, modeset target, dumb framebuffers
  seat.*               seatd session (DRM master + device access, VT switching)
  input.*              libinput + xkb keysym/modifier translation
  window.*             server-owned window model (global coords, z-order, roles)
  wm.*                 window-manager policy (focus, interactive move, Alt-Tab)
  server.*             display server: WM + CPU compositor   (`glacier wm`)
  diag_*.c             Phase-0 platform diagnostics
tests/wm_selftest.c    window-model + WM-policy unit test (no hardware)
```

## Build

```sh
meson setup build      # deps: libdrm gbm egl glesv2 libudev libseat libinput xkbcommon
ninja -C build
meson test -C build    # runs the window/WM unit test
```

## Run

```sh
glacier wm             # the display server — from a bare VT, user in the 'seat' group
                       #   drag a title bar to move · Alt-Tab to switch · Esc to quit
glacier enum           # platform diagnostics: enum|firstlight|flip|gl|seatd|hotplug
```

The modeset diagnostics and `glacier wm` take DRM master, so run them from a bare
VT (stop any display manager first). `enum` and `hotplug` are read-only and safe
under a live session.

## License

Built for the YetiOS project.
