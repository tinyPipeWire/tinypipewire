# tinypipewire

A small C library that wraps PipeWire's `pw_stream` API behind a simpler,
unified interface for capturing audio and video. It hides PipeWire's
thread-loop management, SPA POD format negotiation, and buffer
dequeue/queue plumbing, exposing only a small opaque-handle API.

Audio and camera **capture** are supported, as is audio **playback**.
Video playback is out of scope: PipeWire has no video sink device to play
into, so an application that wants to emit video becomes a source node
instead — which is what a filter's output port already does.

## Build

Requires [Meson](https://mesonbuild.com/), Ninja, and PipeWire development
files (`libpipewire-0.3` >= 0.3.50) discoverable via pkg-config.

```sh
meson setup build
meson compile -C build
meson test -C build
```

`meson test` needs no hardware. Tests that do — they link a real camera,
microphone or temperature sensor into a filter — and tests that measure
timing live in a separate suite that is skipped unless asked for, and
report SKIP rather than failing when a device they need is absent:

```sh
meson test -C build --suite hardware
```

The examples, tests, and utilities each build by default and can be turned
off individually:

```sh
meson setup build -Dexamples=false -Dtests=false -Dutils=false
```

## Quick start

Capture from the default microphone until Ctrl+C:

```c
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include <tpw/tpw_stream.h>

static volatile sig_atomic_t running = 1;

static void on_signal(int sig) { (void)sig; running = 0; }

static void on_data(tpw_stream_h stream, const tpw_stream_buffer* buf, void* user_data)
{
    (void)stream; (void)user_data;
    printf("%zu bytes (pts=%lld ns)\n", buf->size, (long long)buf->pts);
}

int main(void)
{
    signal(SIGINT, on_signal);

    tpw_stream_h s = tpw_stream_create(TPW_DATA_AUDIO, on_data, NULL);
    if (!s)
        return 1;

    tpw_audio_config cfg = { .sample_rate = 48000, .channels = 2 };
    if (tpw_stream_set_audio_config(s, &cfg) != TPW_OK ||
        tpw_stream_start(s) != TPW_OK) {
        tpw_stream_destroy(s);
        return 1;
    }

    while (running)
        sleep(1);

    tpw_stream_stop(s, false);
    tpw_stream_destroy(s);
    return 0;
}
```

Once the library is installed (`meson install -C build`), build against it
with pkg-config:

```sh
cc capture.c $(pkg-config --cflags --libs tinypipewire) -o capture
```

Swap `TPW_DATA_AUDIO` for `TPW_DATA_VIDEO` and
`tpw_stream_set_audio_config()` for `tpw_stream_set_video_config()` to
capture from a camera instead; everything else is the same.

## API

Four headers are installed, one per area plus the types the first two share:

| Header | What it covers | Reference |
| --- | --- | --- |
| `tpw/tpw_types.h` | What a stream or port carries, how a call failed, and the audio, video and DMABUF descriptions | [docs/streams.md](docs/streams.md#error-codes) |
| `tpw/tpw_stream.h` | Audio and video capture, audio playback, choosing a source, manual graph wiring, DMABUF capture | [docs/streams.md](docs/streams.md) |
| `tpw/tpw_filter.h` | Multi-port filters, signal and event ports, DMABUF import and buffer hold, linking a port to a real device | [docs/filters.md](docs/filters.md) |
| `tpw/tpw_log.h` | Redirecting or filtering the library's diagnostics | [docs/logging.md](docs/logging.md) |

Some highlights of what lives behind them:

- **Config structs, not long signatures.** Formats are passed as
  `tpw_audio_config`/`tpw_video_config`, so new fields never change a call.
- **Timestamps.** Every capture buffer carries a `pts` from the driver
  clock; a playback buffer's `pts` says when its first sample will be heard.
- **Zero-copy.** Video capture streams and filter video input ports can both
  receive DMABUF file descriptors instead of CPU-mapped frames.
- **Your own graph.** Streams and filter ports can skip the session manager
  and link themselves to a named device.
- **Ask before you configure.** A camera reports the sizes and frame rates it
  actually has, in a form that goes straight into a `tpw_video_config`. Audio
  has no such call on purpose — PipeWire converts sample formats, rates and
  channel counts for a stream, so whatever you ask for works.

## Examples

Each builds to `./build/examples/`:

| Example | What it shows |
| --- | --- |
| [`audio_capture`](examples/audio_capture.c) | Capture from the default audio source and print each buffer's size |
| [`video_capture`](examples/video_capture.c) | Capture from the default camera and print each frame's size |
| [`video_capture_dmabuf`](examples/video_capture_dmabuf.c) | Capture from the default camera as DMABUF and print each frame's plane fd/stride |
| [`video_capture_mjpeg`](examples/video_capture_mjpeg.c) | Capture MJPEG from the default camera and print each frame's (varying) size |
| [`audio_playback`](examples/audio_playback.c) | Play a generated tone to the default output device, printing when the next samples will be heard. Takes an optional sink name from `wpctl status` |
| [`stream_manual_link`](examples/stream_manual_link.c) | Wire a capture stream to a named device with no session manager involved, pausing so the graph can be inspected before and after. Usage: `stream_manual_link <device> [other-device]`, where the second device re-targets the stream |
| [`list_targets`](examples/list_targets.c) | Print every target `tpw_stream_set_target()` would accept, for audio sources, video sources, and audio sinks — with each camera's supported formats and frame rates |
| [`filter_mix`](examples/filter_mix.c) | Mix two audio input ports into one audio output port |
| [`filter_signal_port`](examples/filter_signal_port.c) | Feed a synthetic signal port alongside an audio port into one filter |
| [`filter_event_port`](examples/filter_event_port.c) | Echo events from an event input port back out through an event output port |
| [`filter_dmabuf_bundle`](examples/filter_dmabuf_bundle.c) | Bundle a DMABUF camera input (with hold) and a faster signal input, printing each frame's fd and freshness |
| [`filter_port_link`](examples/filter_port_link.c) | Link a camera and a microphone straight into a filter by node name, with no `pw-link` step, taking the video port's format from the camera rather than guessing. Usage: `filter_port_link <video-node> [audio-node]` |

Node and device names come from `wpctl status` or `pw-cli ls Node`.

## Utilities

`utils/tpw_record.c` records the default (or a chosen) audio or video
source to a file:

```sh
./build/utils/tpw_record -o out.wav                      # audio, default source, until Ctrl+C
./build/utils/tpw_record -o out.pcm -f pcm -d 10          # raw PCM, 10 seconds
./build/utils/tpw_record -o out.wav --device alsa_input.usb-...  # pick a source (see `wpctl status`)
./build/utils/tpw_record -o out.wav --sample-rate 44100 --channels 1 --bits 24 -d 5  # 44.1kHz mono, 24-bit

./build/utils/tpw_record -o out.raw -t video -d 5         # video, raw I420 frames, 5 seconds
./build/utils/tpw_record -o out.y4m -t video -f y4m --fps 30 -d 5  # playable YUV4MPEG2
./build/utils/tpw_record -o out.raw -t video --width 1280 --height 720 -d 5  # 720p raw I420
./build/utils/tpw_record -o out.raw -t video --device v4l2_input.usb-... -d 5  # pick a camera
```

`utils/tpw_stream_loopback.c` loops captured audio straight to the default
output device and/or logs data callbacks from one or more video capture
streams, using only the `tpw_stream` API. Video streams only log for now;
rendering is a future addition once a UI backend option exists:

```sh
./build/utils/tpw_stream_loopback                                    # mic straight to the default output, until Ctrl+C
./build/utils/tpw_stream_loopback --device alsa_input.usb-... -d 10  # pick a source, 10 seconds
./build/utils/tpw_stream_loopback --sample-rate 44100 --channels 1 --bits 24  # 44.1kHz mono, 24-bit

./build/utils/tpw_stream_loopback --no-audio --video-streams 2 -d 5  # log two default-camera streams, no audio
./build/utils/tpw_stream_loopback --video-streams 1 --dmabuf --fps 30 --width 1280 --height 720  # DMABUF, 720p30
./build/utils/tpw_stream_loopback --video-streams 1 --pixel-format MJPG  # ask the camera for MJPG instead
```

The pixel format is negotiated with the camera as-is, with no conversion in
between, so `--pixel-format` only accepts what the source itself offers. The
default `YUYV` and `MJPG` are the usual safe picks for UVC webcams; an
unsupported format ends the stream with `source lost (error -5)`.

## License

MIT — see [LICENSE](LICENSE).
