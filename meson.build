project('glacier', 'c',
  version: '0.2',
  default_options: ['c_std=c11', 'warning_level=2', 'werror=false'])

cc = meson.get_compiler('c')

wayland_server = dependency('wayland-server')   # Transport B: Wayland frontend

deps = [
  dependency('libdrm'),
  dependency('gbm'),
  dependency('egl'),
  dependency('glesv2'),
  dependency('libudev'),
  dependency('libseat'),
  dependency('libinput'),
  dependency('xkbcommon'),
  wayland_server,
  cc.find_library('m', required: false),
]

inc = include_directories('include')

# ---- Wayland frontend protocols (Phase 4) ----------------------------------
# Generate server-side glue from the stable/unstable XML with wayland-scanner.
# Kept out of git; regenerated at build time, exactly like a wlroots build —
# except we vendor no wlroots, only the frozen core + xdg-shell + xdg-decoration.
wl_scanner = find_program('wayland-scanner')
wl_protos_dir = dependency('wayland-protocols').get_variable(pkgconfig: 'pkgdatadir')

protocols = [
  [wl_protos_dir / 'stable/xdg-shell/xdg-shell.xml', 'xdg-shell'],
  [wl_protos_dir / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml',
   'xdg-decoration'],
]

proto_srcs = []
foreach p : protocols
  xml = p[0]
  name = p[1]
  proto_srcs += custom_target(name + '-server-header',
    input: xml, output: name + '-server-protocol.h',
    command: [wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
  proto_srcs += custom_target(name + '-code',
    input: xml, output: name + '-protocol.c',
    command: [wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
endforeach

# Platform + diagnostics + Phase-2 display server, one binary.
sources = [
  'main.c',
  'src/log.c',
  'src/platform.c',     # DRM/KMS platform: device, modeset target, framebuffers
  'src/seat.c',         # seatd session (DRM master + device access)
  'src/input.c',        # libinput + xkb keysym translation
  'src/window.c',       # server-owned window model
  'src/wm.c',           # window-manager policy (focus, z-order, move, Alt-Tab)
  'src/protocol.c',     # CrystallineLattice wire format: framed I/O + fd passing
  'src/transport.c',    # CrystallineLattice server transport (Path β, Phase 3)
  'src/wayland.c',      # Wayland compatibility frontend (Transport B, Phase 4)
  'src/shell.c',        # Wine desktop-shell launcher (explorer.exe, Path β)
  'src/server.c',       # display server: WM + CPU compositor (`glacier wm`)
  'src/diag_enumerate.c',
  'src/diag_firstlight.c',
  'src/diag_pageflip.c',
  'src/diag_gl.c',
  'src/diag_seat.c',
  'src/diag_hotplug.c',
]

executable('glacier', sources + proto_srcs,
  include_directories: inc,
  dependencies: deps,
  install: true)

# Trivial native CrystallineLattice client — exercises the Path β transport
# without Wine (DESIGN.md Phase 3: build the native test client first).
executable('glacier-client',
  ['src/client.c', 'src/protocol.c'],
  include_directories: inc,
  install: true)

# Unit test for the server-owned window model + WM policy (no hardware).
wm_selftest = executable('wm_selftest',
  ['tests/wm_selftest.c', 'src/window.c', 'src/wm.c'],
  include_directories: inc)
test('wm', wm_selftest)

# Headless test for the CrystallineLattice server transport (no DRM): drives
# the protocol over a socketpair and checks the server-owned window stack.
transport_selftest = executable('transport_selftest',
  ['tests/transport_selftest.c', 'src/protocol.c', 'src/transport.c',
   'src/window.c', 'src/log.c'],
  include_directories: inc)
test('transport', transport_selftest)

# Headless test for the Wayland frontend (no DRM): forks a real libwayland
# client that maps an xdg_toplevel with an shm buffer, and asserts the frontend
# turned it into a server-owned window with the client's pixels.
wayland_client = dependency('wayland-client', required: false)
if wayland_client.found()
  xdg_shell_client_h = custom_target('xdg-shell-client-header',
    input: wl_protos_dir / 'stable/xdg-shell/xdg-shell.xml',
    output: 'xdg-shell-client-protocol.h',
    command: [wl_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
  wayland_selftest = executable('wayland_selftest',
    ['tests/wayland_selftest.c', 'src/wayland.c', 'src/window.c', 'src/log.c',
     proto_srcs, xdg_shell_client_h],
    include_directories: inc,
    dependencies: [wayland_server, wayland_client, dependency('xkbcommon')])
  test('wayland', wayland_selftest)
endif
