project('tinypipewire', 'c',
  version: '0.11.0',
  license: 'MIT',
  default_options: [
    'c_std=c11',
    'warning_level=2',
  ],
)

# SPA headers use glibc extensions (newlocale, ssize_t) that require _GNU_SOURCE.
add_project_arguments('-D_GNU_SOURCE', language: 'c')

# 0.3.50 is where pw_buffer.requested landed; playback needs it to size a cycle.
pipewire_dep = dependency('libpipewire-0.3', version: '>=0.3.50')

tpw_inc = include_directories('include')

subdir('include/tpw')
subdir('src')

tinypipewire_dep = declare_dependency(
  link_with: tinypipewire_lib,
  include_directories: tpw_inc,
  dependencies: [pipewire_dep],
)

install_headers(
  'include/tpw/tpw_stream.h',
  'include/tpw/tpw_filter.h',
  'include/tpw/tpw_log.h',
  'include/tpw/tpw_types.h',
  'include/tpw/tpw_export.h',
  subdir: 'tpw',
)

pkg = import('pkgconfig')
pkg.generate(
  tinypipewire_lib,
  name: 'tinypipewire',
  description: 'Small wrapper around PipeWire pw_stream for audio/video capture and playback',
  requires: 'libpipewire-0.3',
)

if get_option('tests')
  # Hardware tests need a real capture device, so a plain `meson test` skips
  # the whole suite; run it with `meson test --suite hardware`.
  add_test_setup('default', exclude_suites: ['hardware'], is_default: true)
  subdir('tests')
endif

if get_option('examples')
  subdir('examples')
endif

if get_option('utils')
  subdir('utils')
endif
