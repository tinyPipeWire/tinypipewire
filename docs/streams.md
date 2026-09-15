# Streams

`include/tpw/tpw_stream.h` — capturing audio or video from a device, and
playing audio back to one. A stream is a single handle with a single
callback; one `tpw_stream_h` captures either audio or video, and both types
share the same creation, control, and data-callback functions.

- [Capture](#capture)
- [Audio playback](#audio-playback)
- [Wiring a stream yourself](#wiring-a-stream-yourself)
- [DMABUF capture](#dmabuf-capture)

## Capture

```c
tpw_stream_h tpw_stream_create(tpw_stream_type type, tpw_stream_data_cb callback, void* user_data);
int tpw_stream_set_error_cb(tpw_stream_h stream, tpw_stream_error_cb callback);
int tpw_stream_set_target(tpw_stream_h stream, const char* target);
int tpw_stream_set_role(tpw_stream_h stream, const char* role);
int tpw_stream_set_audio_config(tpw_stream_h stream, const tpw_audio_config* config);
int tpw_stream_set_video_config(tpw_stream_h stream, const tpw_video_config* config);
int tpw_stream_start(tpw_stream_h stream);
int tpw_stream_stop(tpw_stream_h stream, bool drain);
void tpw_stream_destroy(tpw_stream_h stream);
```

Format is passed as a config struct, so new fields can be added without
changing the call signature:

```c
tpw_audio_config a = { .sample_rate = 48000, .channels = 2, .format = "F32" };
tpw_stream_set_audio_config(stream, &a);   /* format = NULL defaults to "S16" */

tpw_video_config v = { .width = 640, .height = 480, .pixel_format = "YUYV", .fps = 30 };
tpw_stream_set_video_config(stream, &v);   /* fps = 0 lets the source pick the rate */
```

**Finishing cleanly.** `tpw_stream_stop(stream, false)` pauses immediately,
which can cut off the last buffer or two already queued with the device —
usually a few tens of milliseconds. `tpw_stream_stop(stream, true)` instead
blocks the calling thread until everything already queued has actually been
played (playback) or delivered to the data callback (capture) before
pausing, so nothing is lost. If the device disappears mid-drain, a warning
is logged and the stream stops anyway after a few seconds rather than
blocking forever.

### Choosing a source

By default a stream auto-connects to PipeWire's default source for its
media type. `tpw_stream_set_target()` points it at a specific node by
name or serial instead (see `wpctl status` or `pw-cli ls Node`).
Call it before `tpw_stream_set_audio_config()`/`tpw_stream_set_video_config()`,
which is what actually connects the stream.

`tpw_stream_get_target_list(stream, out, out_len, found)` finds those names
for you, so an application does not have to shell out to `wpctl`/`pw-cli`:

```c
tpw_target_info targets[16];
size_t n = 0;
if (tpw_stream_get_target_list(stream, targets, 16, &n) != TPW_STREAM_OK)
    return; /* the graph could not be reached — distinct from finding nothing */
for (size_t i = 0; i < n && i < 16; i++)
    printf("%s (serial %s) - %s\n", targets[i].name, targets[i].serial, targets[i].description);
```

It lists sources for a capture stream and sinks for a playback stream,
matching `stream`'s own type/direction — the same set `tpw_stream_set_target()`
would accept. Works as soon as the stream is created, before a format is
set. Following a count-then-fill shape: `*found` is the true count, which
may exceed `out_len`, and a NULL `out` asks for the count alone.

The count comes back through a parameter rather than the return value
because this call talks to the server, so it can fail for reasons that have
nothing to do with how many targets exist. An empty graph is
`TPW_STREAM_OK` with `*found` 0; an unreachable one is an error. Queries
that only read buffers already delivered — `tpw_stream_get_dmabuf_planes()`
and the filter's event and DMABUF readers — cannot fail that way, and keep
returning their count directly.

### Declaring a role

`tpw_stream_set_role()` says what the stream is for — `"Music"`, `"Movie"`,
`"Communication"`, `"Notification"` and so on — so a session manager with a
role policy can route or duck it accordingly:

```c
tpw_stream_h s = tpw_stream_create_playback(on_fill, NULL);
tpw_stream_set_role(s, "Notification");   /* before the format, like a target */
tpw_stream_set_audio_config(s, &cfg);
```

The role becomes the node's `media.role` when the format connects the stream,
so set it first; changing it afterwards does not reach the node already
connected. NULL or `""` clears it. The value is a free-form string because the
set of roles belongs to the session manager's configuration, not to PipeWire.

Like a target, it is only a hint, and **nothing checks it**: PipeWire itself
ignores the property, and so does a session manager with no role policy
configured, so an unknown or unused role is `TPW_STREAM_OK` and changes
nothing. Unlike a target it does not contradict manual wiring, so it is
accepted whatever `tpw_stream_set_autoconnect()` says; there it simply labels
the node for anyone inspecting the graph.

It is most useful for audio playback, where role policies usually apply, but
every stream accepts it.

### What a camera can deliver

A video source only produces the sizes and frame rates it actually has:
PipeWire does not scale video, so asking a camera for 1920x1080 when it
tops out at 1280x720 fails to negotiate.
`tpw_stream_get_target_video_formats(stream, target, out, out_len, found)` lists
what a given target has, in a shape meant to be handed straight back:

```c
tpw_video_format_info fmts[32];
size_t n = 0;
if (tpw_stream_get_target_video_formats(stream, "my-camera", fmts, 32, &n) != TPW_STREAM_OK)
    return; /* no such node, or the query timed out */
for (size_t i = 0; i < n && i < 32; i++)
    printf("%s %dx%d @%d\n", fmts[i].pixel_format, fmts[i].width, fmts[i].height,
           fmts[i].n_fps ? fmts[i].fps[0] : 0);

tpw_video_config cfg = {
    .width        = fmts[0].width,
    .height       = fmts[0].height,
    .pixel_format = fmts[0].pixel_format,   /* already a name tpw takes */
    .fps          = fmts[0].n_fps ? fmts[0].fps[0] : 0,
};
tpw_stream_set_video_config(stream, &cfg);
```

Every entry is one `tpw_stream_set_video_config()` accepts, so nothing
needs checking first. A pixel format the camera offers but this library
has no name for is left out rather than reported as something that would
then be rejected. Passing NULL as `target` uses the one
`tpw_stream_set_target()` already set, which is the usual order: pick a
device, ask what it has, configure. `fps` is listed highest first, so
`fps[0]` is the fastest rate at that size.

A device that takes a range of sizes rather than a fixed set reports the
range's ends — `width_max`/`height_max` exceed `width`/`height` — and
anything between them works too. For a discrete size the two are equal.
`n_fps` is what `fps` actually holds, unlike the return value's
count-then-fill rule, so `for (r = 0; r < f->n_fps; r++)` needs no bound
of its own. A device offering more rates than fit keeps its fastest.

Two things to know. Reading a device's formats opens it briefly, so this
is not the free lookup `tpw_stream_get_target_list()` is. And **there is
no audio equivalent, deliberately**: a stream's audio goes through
PipeWire's converter, which resamples, remixes channels and changes
sample formats, so a device's own list would not describe what
`tpw_stream_set_audio_config()` accepts — whatever you ask for works.

### The capture buffer

`tpw_stream_data_cb` receives a `const tpw_stream_buffer*` rather than
loose `data`/`size` parameters, so future fields can be added without
changing the callback signature. It currently carries:

```c
typedef struct {
    void* data;
    size_t size;
    int64_t pts; /* capture timestamp in nanoseconds (the driver clock
                    used by the underlying SPA node, e.g. ALSA or
                    V4L2), or -1 if the buffer had no timestamp
                    metadata. */
} tpw_stream_buffer;
```

The stream requests capture-clock metadata from the source on connect, so
`pts` is populated whenever the source provides one; -1 is the exception
this field exists for, not the common case.

## Audio playback

`tpw_stream_create_playback()` makes a stream that emits to an output device
instead of capturing from an input one. It takes no media type — a video
playback stream cannot be expressed — and everything else is the same as a
capture stream: `tpw_stream_set_target()` picks a specific sink (default
otherwise), `tpw_stream_set_audio_config()` accepts the same formats and
connects, and start/stop/destroy behave identically.
`tpw_stream_set_video_config()` is rejected on it.

```c
tpw_stream_h tpw_stream_create_playback(tpw_stream_playback_cb callback, void* user_data);
```

The difference is the callback. Capture hands you a `const` buffer to read;
playback hands you a writable one to fill and asks how much you wrote:

```c
typedef struct {
    void* data;       /* writable region for this cycle */
    size_t available; /* bytes you may write this cycle: what the device asked
                         for, or the region's full size if the graph said
                         nothing. Not the region's capacity — usually less. */
    int64_t pts;      /* when this cycle's first sample is expected to be
                         *heard*, in monotonic nanoseconds, or -1. The mirror
                         of the capture pts: capture says when samples were
                         taken, playback when they will be played — which is
                         what you sync other media against. */
    size_t size;      /* you set this: bytes actually written */
} tpw_stream_playback_buffer;
```

A full cycle always goes out. Write less than `available` and the remainder is
emitted as silence; write nothing and the cycle is silent but the stream keeps
running. An oversized `size` is clamped, a count that is not a whole number of
frames is floored, and both are noted in the log.

The callback runs on the real-time data thread: it must not block, allocate,
or perform I/O. A cycle whose callback overruns its budget is emitted as
silence and logged — rate-limited to one report per second, so a persistently
slow callback does not drown the log — and is never reported through the error
callback, which stays reserved for the output device disappearing.
`tpw_stream_stop(stream, true)` (see [Capture](#capture)) drains the
device the same way as for a capture stream.

## Wiring a stream yourself

By default a stream declares itself for automatic connection and the session
manager (WirePlumber, typically) decides what it links to; `tpw_stream_set_target()`
is a *hint* to that decision. On a system running no session manager, nothing
makes the decision and the stream connects to nothing at all.

Three calls let an application do the wiring instead:

```c
int tpw_stream_set_autoconnect(tpw_stream_h stream, bool enable);
int tpw_stream_link(tpw_stream_h stream, const char* target);
int tpw_stream_unlink(tpw_stream_h stream);
```

```c
tpw_stream_h s = tpw_stream_create(TPW_STREAM_TYPE_AUDIO, on_data, NULL);
tpw_stream_set_autoconnect(s, false);        /* before the format */
tpw_stream_set_audio_config(s, &cfg);
tpw_stream_start(s);                          /* the graph is where we look */
tpw_stream_link(s, "alsa_input.usb-046d_C922...analog-stereo");
```

`tpw_stream_link()` comes *after* `tpw_stream_start()`, unlike every other
setup call: both the target and the stream's own ports are resolved in the
running graph, and the ports appear a moment after the stream starts. Channels
are paired by position, so a stereo stream reaches a stereo device's two ports
without naming any of them. The call blocks until every link negotiates, and if
any channel fails none is left behind.

A device with **fewer** channels than the stream is rejected outright — a
stream never ends up half-wired. A device with **more** succeeds, leaving the
surplus unconnected and saying so in the log, since a mono stream reaching one
side of a stereo device otherwise looks like a fault.

Links live from `tpw_stream_link()` until `tpw_stream_unlink()` or
`tpw_stream_destroy()`. **`tpw_stream_stop()` does not release them**, so a
stopped stream resumes on the same device rather than silently running
unconnected.

Automatic connection stays on unless you turn it off, so existing code is
unaffected. The two modes are mutually exclusive: combining
`tpw_stream_set_target()` with `tpw_stream_set_autoconnect(false)` returns
`TPW_STREAM_ERR_INVALID_ARG`, whichever you call second.

To move from one mode to the other before the format connects the stream,
clear the target first: `tpw_stream_set_target(stream, NULL)` is accepted
whatever the autoconnect setting, and is what leaves `tpw_stream_set_autoconnect()`
free to go either way. Turning autoconnect back on does not clear a target
by itself, so a stream that had one keeps being pointed at it.

### What manual wiring takes on

Choosing manual wiring takes on three things the session manager otherwise
handles:

- **Channel order.** Pairing is positional, and an unnegotiated stream's ports
  carry no channel identity, so the library cannot verify that your channel 0
  is the left channel.
- **Reconnection.** When a linked device disappears the error callback fires
  with `TPW_STREAM_ERR_SOURCE_UNAVAILABLE` and nothing re-routes; linking
  somewhere else is your decision.
- **Device availability.** On a system with no session manager the device nodes
  themselves must come from somewhere — the daemon's own configuration, for
  instance. This controls wiring, not what devices exist.

## Calling the library from a callback

Each callback runs on a thread the library owns, and some calls cannot be made
from there without deadlocking or tearing down the thread they run on. Those
calls are refused with `TPW_STREAM_ERR_INVALID_ARG` and the reason is logged;
`tpw_stream_destroy()` has no return value, so it only logs and leaves the
stream as it was.

| Callback | Runs on | Refused inside it |
| --- | --- | --- |
| data, playback | PipeWire's real-time data thread | start, stop, destroy, link, unlink, the target queries, the format setters |
| error | the stream's loop thread | destroy, link, the target queries, the format setters, a draining stop |

A stop without drain and `tpw_stream_unlink()` work in the error callback, and
the setters that only record a value (`tpw_stream_set_target()`,
`tpw_stream_set_role()` and the like) work anywhere. To re-route after losing
a device, leave the error callback first: note what happened there, then link
from your own thread.

## DMABUF capture

A video capture stream can opt into receiving DMABUF file descriptors
instead of CPU-mapped frames, so a single-consumer application (an encoder,
a GPU import path) never has to build a `tpw_filter` just to forward one
input:

```c
typedef struct { tpw_port_memory memory; } tpw_stream_dmabuf_opts;

int tpw_stream_set_video_config_ex(tpw_stream_h stream, const tpw_video_config* config,
                                    const tpw_stream_dmabuf_opts* opts);
size_t tpw_stream_get_dmabuf_planes(tpw_stream_h stream, tpw_dmabuf_plane* planes, size_t planes_len);
```

`tpw_stream_set_video_config_ex()` with `opts->memory == TPW_PORT_MEMORY_DMABUF`
requests DMABUF-capable capture; `opts == NULL` is exactly
`tpw_stream_set_video_config()`, unchanged. On a DMABUF stream every
delivered `tpw_stream_buffer.data` is NULL — read the frame's planes with
`tpw_stream_get_dmabuf_planes()`, which returns the plane count and fills
`fd`/`offset`/`stride`/`size` per plane (one for RGB/YUYV, more for planar
formats like NV12/I420). It returns 0 for a non-DMABUF stream, never
fabricating an fd, and the `fd` is borrowed for the callback only. If the
source cannot provide DMABUF, the stream delivers no frames and
`tpw_stream_error_cb` fires with `TPW_STREAM_ERR_SOURCE_UNAVAILABLE` —
there is no silent fallback to CPU-mapped delivery. This capability is
video-capture-only; requesting it on an audio or playback stream is
rejected the same way an ordinary video config is.

## See also

- [Filters](filters.md) — combining several sources into one processed output
- [Logging](logging.md) — redirecting the library's diagnostics
