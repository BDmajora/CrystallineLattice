project('glacier-phase0', 'c',
  version: '0.0',
  default_options: ['c_std=c11', 'warning_level=2', 'werror=false'])

cc = meson.get_compiler('c')

deps = [
  dependency('libdrm'),
  dependency('gbm'),
  dependency('egl'),
  dependency('glesv2'),
  dependency('libudev'),
  dependency('libseat'),
  cc.find_library('m', required: false),
]

sources = [
  'main.c',
  'src/common.c',
  'src/p0_1_enum.c',
  'src/p0_2_firstlight.c',
  'src/p0_3_flip.c',
  'src/p0_4_gl.c',
  'src/p0_5_seatd.c',
  'src/p0_6_hotplug.c',
]

executable('glacier-phase0', sources,
  include_directories: include_directories('include'),
  dependencies: deps,
  install: true)
