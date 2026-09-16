/* SPDX-License-Identifier: MIT */

/* The fill rules are pure buffer arithmetic, so they are exercised through
 * tpw_stream_playback_fill() directly — no output device, no graph, no
 * timing — the way the filter tests already drive tpw_filter_on_process().
 * A whitebox format setup avoids connecting, so these run on a machine with
 * no sink at all. */

#include <string.h>

#include "tpw_stream_internal.h"
#include "tpw_test.h"

#define RATE 48000
#define CHANNELS 2
#define FRAME (2 * CHANNELS) /* S16 stereo */

static size_t g_available;
static void* g_data;
static int64_t g_pts;
static int g_calls;
static size_t g_report; /* what the callback claims it wrote */

static void fill_cb(tpw_stream_h stream, tpw_stream_playback_buffer* buf, void* user_data)
{
    (void)stream;
    (void)user_data;
    g_calls++;
    g_available = buf->available;
    g_data = buf->data;
    g_pts = buf->pts;
    if (g_report > 0)
        memset(buf->data, 0xAB, g_report > buf->available ? buf->available : g_report);
    buf->size = g_report;
}

/* A playback stream with a format but no connection, so no device is
 * needed. tpw_stream_set_audio_config() would connect; this does not. */
static struct tpw_stream* make_stream(void)
{
    tpw_stream_h handle = tpw_stream_create_playback(fill_cb, NULL);
    TPW_ASSERT(handle != NULL);
    struct tpw_stream* stream = (struct tpw_stream*)handle;
    stream->format.audio.sample_rate = RATE;
    stream->format.audio.channels = CHANNELS;
    stream->format.audio.format = SPA_AUDIO_FORMAT_S16;
    stream->bytes_per_frame = FRAME;
    return stream;
}

static void test_frame_size_helper(void)
{
    TPW_ASSERT_EQ(tpw_audio_bytes_per_frame(SPA_AUDIO_FORMAT_U8, 2), (size_t)2);
    TPW_ASSERT_EQ(tpw_audio_bytes_per_frame(SPA_AUDIO_FORMAT_S16, 2), (size_t)4);
    TPW_ASSERT_EQ(tpw_audio_bytes_per_frame(SPA_AUDIO_FORMAT_S24, 1), (size_t)3);
    TPW_ASSERT_EQ(tpw_audio_bytes_per_frame(SPA_AUDIO_FORMAT_S32, 2), (size_t)8);
    TPW_ASSERT_EQ(tpw_audio_bytes_per_frame(SPA_AUDIO_FORMAT_F32, 6), (size_t)24);
    TPW_ASSERT_EQ(tpw_audio_bytes_per_frame(SPA_AUDIO_FORMAT_S24_32, 2), (size_t)8);
    TPW_ASSERT_EQ(tpw_audio_bytes_per_frame(SPA_AUDIO_FORMAT_S16, 0), (size_t)0);
    TPW_ASSERT_EQ(tpw_audio_bytes_per_frame(SPA_AUDIO_FORMAT_UNKNOWN, 2), (size_t)0);
}

/* The callback gets a writable region and the byte count it may use, and a
 * full fill is published whole. */
static void test_full_fill(void)
{
    struct tpw_stream* stream = make_stream();
    unsigned char region[FRAME * 16];

    g_calls = 0;
    g_report = sizeof(region);
    size_t written = tpw_stream_playback_fill(stream, region, sizeof(region), 1234);

    TPW_ASSERT_EQ(g_calls, 1);
    TPW_ASSERT(g_data == region);
    TPW_ASSERT_EQ(g_available, sizeof(region));
    TPW_ASSERT_EQ(g_pts, (int64_t)1234);
    TPW_ASSERT_EQ(written, sizeof(region));
    for (size_t i = 0; i < sizeof(region); i++)
        TPW_ASSERT_EQ(region[i], 0xAB);

    tpw_stream_destroy((tpw_stream_h)stream);
}

/* A short fill keeps what the callback wrote and silences the rest. */
static void test_short_fill_is_silenced(void)
{
    struct tpw_stream* stream = make_stream();
    unsigned char region[FRAME * 16];
    memset(region, 0xFF, sizeof(region));

    g_report = FRAME * 4;
    size_t written = tpw_stream_playback_fill(stream, region, sizeof(region), -1);

    TPW_ASSERT_EQ(written, (size_t)(FRAME * 4));
    for (size_t i = 0; i < FRAME * 4; i++)
        TPW_ASSERT_EQ(region[i], 0xAB);
    for (size_t i = FRAME * 4; i < sizeof(region); i++)
        TPW_ASSERT_EQ(region[i], 0x00); /* not the 0xFF left behind */

    tpw_stream_destroy((tpw_stream_h)stream);
}

/* Reporting nothing emits a silent cycle rather than stopping. */
static void test_zero_fill_is_all_silence(void)
{
    struct tpw_stream* stream = make_stream();
    unsigned char region[FRAME * 8];
    memset(region, 0xFF, sizeof(region));

    g_calls = 0;
    g_report = 0;
    size_t written = tpw_stream_playback_fill(stream, region, sizeof(region), -1);

    TPW_ASSERT_EQ(g_calls, 1);
    TPW_ASSERT_EQ(written, (size_t)0);
    for (size_t i = 0; i < sizeof(region); i++)
        TPW_ASSERT_EQ(region[i], 0x00);

    /* The stream is still usable: a second cycle runs normally. */
    g_report = FRAME;
    TPW_ASSERT_EQ(tpw_stream_playback_fill(stream, region, sizeof(region), -1), (size_t)FRAME);

    tpw_stream_destroy((tpw_stream_h)stream);
}

/* An oversized report is clamped, and nothing past the region is read
 * or written — the guard bytes after it must survive untouched. */
static void test_oversized_report_is_clamped(void)
{
    struct tpw_stream* stream = make_stream();
    unsigned char backing[FRAME * 12];
    memset(backing, 0x5A, sizeof(backing));

    size_t available = FRAME * 8;
    g_report = available * 4;
    size_t written = tpw_stream_playback_fill(stream, backing, available, -1);

    TPW_ASSERT_EQ(written, available);
    for (size_t i = available; i < sizeof(backing); i++)
        TPW_ASSERT_EQ(backing[i], 0x5A);

    tpw_stream_destroy((tpw_stream_h)stream);
}

/* A count that is not a whole number of frames is floored, and the partial
 * frame is silenced rather than emitted. */
static void test_partial_frame_is_truncated(void)
{
    struct tpw_stream* stream = make_stream();
    unsigned char region[FRAME * 8];
    memset(region, 0xFF, sizeof(region));

    g_report = FRAME * 3 + 1;
    size_t written = tpw_stream_playback_fill(stream, region, sizeof(region), -1);

    TPW_ASSERT_EQ(written, (size_t)(FRAME * 3));
    TPW_ASSERT_EQ(region[FRAME * 3], 0x00);

    tpw_stream_destroy((tpw_stream_h)stream);
}

/* Direction is decided at creation: the existing constructor still makes a
 * capture stream, and a video format has nothing to connect to on playback. */
static void test_direction_and_video_rejection(void)
{
    tpw_stream_h playback = tpw_stream_create_playback(fill_cb, NULL);
    TPW_ASSERT(playback != NULL);
    TPW_ASSERT_EQ(((struct tpw_stream*)playback)->direction, TPW_STREAM_DIRECTION_PLAYBACK);
    TPW_ASSERT_EQ(((struct tpw_stream*)playback)->type, TPW_DATA_AUDIO);

    tpw_video_config vcfg = { .width = 640, .height = 480, .pixel_format = "I420", .fps = 30 };
    TPW_ASSERT_EQ(tpw_stream_set_video_config(playback, &vcfg), TPW_ERR_INVALID_ARG);
    TPW_ASSERT(!((struct tpw_stream*)playback)->format_set);
    tpw_stream_destroy(playback);

    TPW_ASSERT(tpw_stream_create_playback(NULL, NULL) == NULL);
}

static void capture_cb(tpw_stream_h stream, const tpw_stream_buffer* buf, void* user_data)
{
    (void)stream;
    (void)buf;
    (void)user_data;
}

static void test_existing_constructor_is_still_capture(void)
{
    tpw_stream_h audio = tpw_stream_create(TPW_DATA_AUDIO, capture_cb, NULL);
    TPW_ASSERT(audio != NULL);
    TPW_ASSERT_EQ(((struct tpw_stream*)audio)->direction, TPW_STREAM_DIRECTION_CAPTURE);
    tpw_stream_destroy(audio);

    tpw_stream_h video = tpw_stream_create(TPW_DATA_VIDEO, capture_cb, NULL);
    TPW_ASSERT(video != NULL);
    TPW_ASSERT_EQ(((struct tpw_stream*)video)->direction, TPW_STREAM_DIRECTION_CAPTURE);
    tpw_stream_destroy(video);
}

/* Naming an output device is the same pre-connect step it is for capture,
 * and clearing it returns the stream to the default device. */
static void test_target_selection(void)
{
    tpw_stream_h handle = tpw_stream_create_playback(fill_cb, NULL);
    TPW_ASSERT(handle != NULL);
    struct tpw_stream* stream = (struct tpw_stream*)handle;

    TPW_ASSERT(stream->target == NULL); /* default device until told otherwise */

    TPW_ASSERT_EQ(tpw_stream_set_target(handle, "alsa_output.some-sink"), TPW_OK);
    TPW_ASSERT(stream->target != NULL);
    TPW_ASSERT_EQ(strcmp(stream->target, "alsa_output.some-sink"), 0);

    TPW_ASSERT_EQ(tpw_stream_set_target(handle, NULL), TPW_OK);
    TPW_ASSERT(stream->target == NULL);

    tpw_stream_destroy(handle);
}

/* Starting before a format is configured is refused, the same way it is for
 * capture, so a playback stream cannot run without a negotiated cycle. */
static void test_start_requires_a_format(void)
{
    tpw_stream_h handle = tpw_stream_create_playback(fill_cb, NULL);
    TPW_ASSERT(handle != NULL);

    TPW_ASSERT_EQ(tpw_stream_start(handle), TPW_ERR_NOT_CONFIGURED);
    TPW_ASSERT_EQ(tpw_stream_stop(handle, false), TPW_OK); /* stopping an idle stream is a no-op */

    tpw_stream_destroy(handle);
}

/* Repeated overruns collapse into one report per interval instead of one
 * per cycle, and the suppressed ones are counted rather than lost. */
static void test_overrun_log_is_rate_limited(void)
{
    struct tpw_stream* stream = make_stream();
    const uint64_t second = 1000000000ull;

    TPW_ASSERT(tpw_stream_playback_note_overrun(stream, second));      /* first always reports */
    TPW_ASSERT(!tpw_stream_playback_note_overrun(stream, second + 1));
    TPW_ASSERT(!tpw_stream_playback_note_overrun(stream, second + 2));
    TPW_ASSERT_EQ(stream->overrun_suppressed, (uint64_t)2);

    TPW_ASSERT(tpw_stream_playback_note_overrun(stream, second * 3)); /* interval elapsed */
    TPW_ASSERT_EQ(stream->overrun_suppressed, (uint64_t)2); /* the caller clears after logging */

    tpw_stream_destroy((tpw_stream_h)stream);
}

int main(void)
{
    test_frame_size_helper();
    test_full_fill();
    test_short_fill_is_silenced();
    test_zero_fill_is_all_silence();
    test_oversized_report_is_clamped();
    test_partial_frame_is_truncated();
    test_direction_and_video_rejection();
    test_existing_constructor_is_still_capture();
    test_target_selection();
    test_start_requires_a_format();
    test_overrun_log_is_rate_limited();
    printf("test_stream_playback: all cases passed\n");
    return 0;
}
