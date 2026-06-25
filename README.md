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
- **Phase 1 — compositor primitives: partial.** A CPU (pixman-style) compositor
  draws the scene and server-side decorations. **Deferred:** GL composition and
  the KMS hardware-cursor plane — `glacier wm` currently paints a *software*
  cursor and CPU-composites instead.
- **Phase 2 — input + server-authoritative WM: done.** The server-owned window
  model, WM policy (focus, z-order, interactive move, Alt-Tab), input routing in
  the global virtual-screen space, and `glacier wm` are landed. These are the
  real server-owned mechanisms, not placeholders. Both relative *and* absolute
  pointer motion are handled — the latter is what mice on QEMU/VM tablets emit,
  so the cursor is no longer frozen under virtualization.
- **Phase 3 — CrystallineLattice transport (Path β): in progress.** The native,
  server-authoritative client↔server protocol and its server side have landed:
  a SOCK_SEQPACKET rendezvous socket, the v0 wire format with a min-version
  handshake, explicit window **roles** (no geometry heuristics), `SCM_RIGHTS`
  buffer passing (shm/memfd today, dma-buf next) with the render-fence fd
  already on the wire, and crash-resilient per-client reaping. A trivial native
  test client, **`glacier-client`**, drives it without Wine. Client windows join
  the same scene as the demo surfaces, wearing the server-drawn title bar.
  **Next:** routing input to the focused client, dma-buf/EGLImage import, then
  `winedrm.drv` (the first real Wine client) and the Phase 1 GL/hardware-cursor
  back-fill.

> **Black screen / respawn loop?** That means `glacier wm` is exiting during
> startup, not that a client is missing — the demo scene paints as soon as init
> survives. Read the session's own log for the failing step:
> `~/.snowfall-session.log` (and `~/glacier.log` if the debug wrapper is in
> place); the `[ERR]` line names which init stage bailed (seat, KMS, fb, input).
> Input is now non-fatal, so a missing mouse/keyboard no longer blanks the
> desktop.

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
  protocol.*           CrystallineLattice wire format: framed I/O + fd passing
  transport.*          CrystallineLattice server transport   (Path β, Phase 3)
  server.*             display server: WM + CPU compositor   (`glacier wm`)
  client.c             glacier-client: trivial native test client (no Wine)
  diag_*.c             Phase-0 platform diagnostics
tests/wm_selftest.c          window-model + WM-policy unit test (no hardware)
tests/transport_selftest.c   transport protocol test over a socketpair (no DRM)
```

## Build

```sh
meson setup build      # deps: libdrm gbm egl glesv2 libudev libseat libinput xkbcommon
ninja -C build
meson test -C build    # runs the window/WM and transport unit tests (no hardware)
```

## Run

```sh
glacier wm             # the display server — from a bare VT, user in the 'seat' group
                       #   drag a title bar to move · Alt-Tab to switch · Esc to quit
glacier-client         # a native CrystallineLattice client: opens one window in `wm`
glacier enum           # platform diagnostics: enum|firstlight|flip|gl|seatd|hotplug
```

The modeset diagnostics and `glacier wm` take DRM master, so run them from a bare
VT (stop any display manager first). `enum` and `hotplug` are read-only and safe
under a live session. With `glacier wm` running, launch `glacier-client` from any
terminal in the same session (it finds the server via `$XDG_RUNTIME_DIR/glacier-0`,
overridable with `$GLACIER_SOCKET`) to see a client window appear in the scene.

## License

Built for the YetiOS project.
