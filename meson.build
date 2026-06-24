project('glacier', 'c',
  version: '0.2',
  default_options: ['c_std=c11', 'warning_level=2', 'werror=false'])

cc = meson.get_compiler('c')

deps = [
  dependency('libdrm'),
  dependency('gbm'),
  dependency('egl'),
  dependency('glesv2'),
  dependency('libudev'),
  dependency('libseat'),
  dependency('libinput'),
  dependency('xkbcommon'),
  cc.find_library('m', required: false),
]

inc = include_directories('include')

# Platform + diagnostics + Phase-2 display server, one binary.
sources = [
  'main.c',
  'src/log.c',
  'src/platform.c',     # DRM/KMS platform: device, modeset target, framebuffers
  'src/seat.c',         # seatd session (DRM master + device access)
  'src/input.c',        # libinput + xkb keysym translation
  'src/window.c',       # server-owned window model
  'src/wm.c',           # window-manager policy (focus, z-order, move, Alt-Tab)
  'src/server.c',       # display server: WM + CPU compositor (`glacier wm`)
  'src/diag_enumerate.c',
  'src/diag_firstlight.c',
  'src/diag_pageflip.c',
  'src/diag_gl.c',
  'src/diag_seat.c',
  'src/diag_hotplug.c',
]

executable('glacier', sources,
  include_directories: inc,
  dependencies: deps,
  install: true)

# Unit test for the server-owned window model + WM policy (no hardware).
wm_selftest = executable('wm_selftest',
  ['tests/wm_selftest.c', 'src/window.c', 'src/wm.c'],
  include_directories: inc)
test('wm', wm_selftest)
