unit_tests = [
  'test_stream_lifecycle',
  'test_stream_target',
  'test_audio_format',
  'test_video_format',
  'test_error_notification',
  'test_filter_ports',
  'test_filter_process',
  'test_filter_error_notification',
  'test_filter_push_data',
  'test_log',
  'test_filter_signal_port',
  'test_filter_event_port',
  'test_filter_port_type',
  'test_filter_dmabuf',
  'test_filter_hold',
  'test_filter_period_hint',
  'test_filter_port_link',
  'test_filter_callback_push',
  'test_filter_push_order',
  'test_filter_push_race',
  'test_stream_playback',
  'test_stream_routing',
  'test_stream_role',
  'test_stream_dmabuf',
  'test_video_enum',
]

# test_error_notification.c is a whitebox test: it reaches into
# tpw_stream_internal.h to simulate a state transition (see that file).
src_inc = include_directories('../src')

foreach name : unit_tests
  exe = executable(name, name + '.c',
    dependencies: [tinypipewire_dep],
    include_directories: src_inc,
  )
  test(name, exe)
endforeach

# Tests needing real hardware, or measuring timing. They exit 77 (SKIP)
# when the device they need is absent, and are kept out of a plain
# `meson test` run: use `meson test --suite hardware` to run them.
hardware_tests = [
  'test_filter_port_link_hw',
  'test_filter_sensor_latency_hw',
  'test_filter_sensor_dummy_hw',
  'test_stream_playback_hw',
  'test_stream_routing_hw',
  'test_stream_dmabuf_hw',
  'test_stream_capture_hw',
  'test_video_enum_hw',
]

foreach name : hardware_tests
  exe = executable(name, [name + '.c', 'tpw_test_hw_discover.c'],
    dependencies: [tinypipewire_dep],
    include_directories: src_inc,
  )
  # A camera is an exclusive device, so these must not run against each
  # other or the second one fails to link.
  test(name, exe, suite: 'hardware', timeout: 60, is_parallel: false)
endforeach

# Doxygen convention check on the public headers: out-params last (an
# out-array may be immediately followed by its own `_len` companion),
# @param names matching the declaration, @brief/@return present, and
# every @see resolving to a real symbol. Same script CI runs on PRs.
python3_prog = find_program('python3', required: false)
if python3_prog.found()
  test('check_doxygen_conventions', python3_prog,
    args: [
      files('../.github/scripts/check_doxygen_conventions.py'),
      meson.project_source_root() / 'include' / 'tpw' / 'tpw_stream.h',
      meson.project_source_root() / 'include' / 'tpw' / 'tpw_filter.h',
    ],
  )
endif
