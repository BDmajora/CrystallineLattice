# glacier — Phase 0 (platform bring-up)

Six P0.x sub-phases over raw DRM/KMS, in strict dependency order, built as
**one binary** with a subcommand per sub-phase. No wlroots, no Wayland.

```
glacier-phase0 <enum|firstlight|flip|gl|seatd|hotplug> [/dev/dri/cardN]
```

| Subcommand   | Sub-phase                | Master? | Gate |
|--------------|--------------------------|---------|------|
| `enum`       | P0.1 enumeration         | no      | dump matches `drm_info`/`modetest` |
| `firstlight` | P0.2 dumb buffer modeset | yes     | full-screen solid color |
| `flip`       | P0.3 page-flip loop      | yes     | tear-free moving rectangle |
| `gl`         | P0.4 GBM/EGL/GLES        | yes     | GL triangle scanned out |
| `seatd`      | P0.5 seatd + VT switch   | seat    | survives Ctrl-Alt-Fn away/back |
| `hotplug`    | P0.6 hotplug             | no      | re-enumerates on uevent |

## Layout
```
main.c              dispatcher (subcommand -> p0_*_run)
include/common.h    shared platform API
include/phases.h    p0_*_run declarations
src/common.c        device open, atomic prop maps, KMS target, dumb FB
src/p0_1_enum.c ..  one sub-phase per file (each defines p0_*_run)
test/run-phase0.sh  build + non-destructive checks + VT instructions
```
The GL path keeps the dumb buffer as the pixman/CPU fallback.

## Build deps (Gentoo)
`x11-libs/libdrm media-libs/mesa[gbm,egl,gles2] virtual/libudev
sys-auth/seatd dev-build/meson`.

## Build & test
```
meson setup build
ninja -C build
test/run-phase0.sh                  # auto-runs enum + hotplug; prints VT steps
test/run-phase0.sh /dev/dri/card1   # pin a device
```

## QEMU virtio-gpu (CI/dev loop)
Boot with `-device virtio-gpu-pci` (or `-vga virtio`) to a text console,
then run subcommands on the VT. virtio may need `virgl`/llvmpipe for `gl`;
if EGL/GLES is unavailable, `enum` still passes and `gl` is fallback-only
for that target. Hotplug test: `device_del`/`device_add` a `virtio-gpu-pci`
from the QEMU monitor.

## Notes
- `firstlight`/`flip`/`gl` run as root on a bare VT (seatd arrives at the
  `seatd` step). Stop any display manager / compositor first — it holds
  DRM master.
- `seatd` needs `seatd` running and your user in the `seat` group.
- Each modeset subcommand auto-exits (5s / 15s) or on Ctrl-C.
