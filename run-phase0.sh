#!/usr/bin/env bash
# Phase 0 test harness for the single `glacier-phase0` binary.
#
# Non-destructive subcommands (enum, hotplug) run anywhere — including
# inside an X/Wayland session — because they never take DRM master.
#
# The modeset subcommands (firstlight, flip, gl, seatd) take over the
# display and MUST run from a bare VT with no compositor holding master:
#     Ctrl-Alt-F3, log in, stop any display-manager, then run the binary.
# In QEMU just boot to a text console and run them there.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
BIN="$BUILD/glacier-phase0"
DEV="${1:-}"   # optional explicit /dev/dri/cardN

echo "== build =="
meson setup "$BUILD" "$ROOT" >/dev/null 2>&1 || meson setup --reconfigure "$BUILD" "$ROOT"
ninja -C "$BUILD" || { echo "build failed"; exit 1; }

echo
echo "== P0.1: enumeration vs reference tools =="
"$BIN" enum $DEV | tee /tmp/glacier_p0_1.txt

if command -v drm_info >/dev/null; then
	echo "--- drm_info (compare connector/plane/crtc counts + types) ---"
	drm_info 2>/dev/null | grep -Ei 'connector|plane|crtc' | head -n 40
else
	echo "(install drm_info to cross-check: counts, plane types, modes)"
fi
if command -v modetest >/dev/null; then
	echo "--- modetest -c -p (connectors + planes) ---"
	modetest ${DEV:+-M ${DEV##*/}} -c -p 2>/dev/null | head -n 40
else
	echo "(install libdrm-tests/modetest to cross-check planes)"
fi
echo "GATE P0.1: connector/CRTC/plane counts + plane types match the tools."

echo
echo "== P0.6: hotplug (10s window) =="
echo "Plug/unplug a monitor now, or: (qemu) device_del/device_add a virtio-gpu."
timeout 10 "$BIN" hotplug $DEV || true
echo "GATE P0.6: each hotplug logs an event + re-enumerates without crashing."

echo
cat <<USAGE
== Modeset subcommands — run from a bare VT (not under this session) ==
  sudo $BIN firstlight $DEV   # GATE: full-screen solid color, 5s
  sudo $BIN flip       $DEV   # GATE: smooth tear-free moving rect, 15s
  sudo $BIN gl         $DEV   # GATE: animated GL triangle scanned out, 15s
       $BIN seatd      ${DEV:-/dev/dri/card0}   # as a 'seat'-group user;
                               # GATE: green screen survives Ctrl-Alt-F2 away/back

Phase 0 kill-gate: on every in-scope target you come up unprivileged via
seatd (seatd), set a mode (firstlight), run a vsync'd GL loop with CPU
fallback (flip/gl), and survive VT switch (seatd) + hotplug (hotplug).
USAGE
