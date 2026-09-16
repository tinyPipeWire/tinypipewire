/* SPDX-License-Identifier: MIT */

/* Needs a real output device, and exits 77 to skip without one. Assertions
 * are deliberately loose — how many cycles run in a given wall-clock window
 * depends on the machine's quantum and sample rate. */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <tpw/tpw_stream.h>

#include "tpw_test.h"
#include "tpw_test_hw_discover.h"

#define TEST_SKIP 77

/* Long enough for the graph to settle and deliver a good many cycles. */
#define RUN_USEC (1000 * 1000)

#define RATE 48000
#define CHANNELS 2

struct counters {
    unsigned cycles;
    size_t bytes_asked;
    bool saw_null_region;
    bool saw_zero_available;
    bool saw_pts;
    bool pts_advanced;
    int64_t last_pts;
    int error_code;
};

/* Writes a full cycle of a cheap square wave: audible, and trivial enough
 * to stay well inside the cycle budget on any machine. */
static void on_fill(tpw_stream_h stream, tpw_stream_playback_buffer* buf, void* user_data)
{
    (void)stream;
    struct counters* c = user_data;
    c->cycles++;
    c->bytes_asked += buf->available;

    if (!buf->data)
        c->saw_null_region = true;
    if (buf->available == 0)
        c->saw_zero_available = true;

    if (buf->pts >= 0) {
        if (c->saw_pts && buf->pts > c->last_pts)
            c->pts_advanced = true;
        c->saw_pts = true;
        c->last_pts = buf->pts;
    }

    if (buf->data && buf->available > 0) {
        static int16_t level = 3000;
        int16_t* out = buf->data;
        size_t frames = buf->available / (sizeof(int16_t) * CHANNELS);
        for (size_t i = 0; i < frames; i++) {
            if ((i % 64) == 0)
                level = (int16_t)-level;
            for (int ch = 0; ch < CHANNELS; ch++)
                *out++ = level;
        }
        buf->size = frames * sizeof(int16_t) * CHANNELS;
    }
}

static void on_error(tpw_stream_h stream, int error_code, void* user_data)
{
    (void)stream;
    struct counters* c = user_data;
    c->error_code = error_code;
}

int main(void)
{
    char sink[256];
    if (!tpw_test_find_node("Audio/Sink", sink, sizeof(sink))) {
        printf("no audio sink present, skipping\n");
        return TEST_SKIP;
    }
    printf("playing to sink: %s\n", sink);

    struct counters c = { .last_pts = -1 };

    tpw_stream_h stream = tpw_stream_create_playback(on_fill, &c);
    if (!stream) {
        printf("no PipeWire connection, skipping\n");
        return TEST_SKIP;
    }

    TPW_ASSERT_EQ(tpw_stream_set_error_cb(stream, on_error), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_set_target(stream, sink), TPW_OK);

    tpw_audio_config cfg = { .sample_rate = RATE, .channels = CHANNELS, .format = "S16" };
    TPW_ASSERT_EQ(tpw_stream_set_audio_config(stream, &cfg), TPW_OK);
    TPW_ASSERT_EQ(tpw_stream_start(stream), TPW_OK);

    usleep(RUN_USEC);

    TPW_ASSERT_EQ(tpw_stream_stop(stream, false), TPW_OK);

    printf("cycles=%u bytes=%zu pts=%s\n", c.cycles, c.bytes_asked,
           c.saw_pts ? "yes" : "no");

    /* The device pulled real cycles, and every one of them offered a usable
     * region — the whole point of the feature. */
    TPW_ASSERT(c.cycles > 0);
    TPW_ASSERT(c.bytes_asked > 0);
    TPW_ASSERT(!c.saw_null_region);
    TPW_ASSERT(!c.saw_zero_available);
    TPW_ASSERT_EQ(c.error_code, 0);

    /* A running sink knows when its samples will be heard, and that time
     * moves forward as cycles go by. */
    TPW_ASSERT(c.saw_pts);
    TPW_ASSERT(c.pts_advanced);

    /* Stopping halts the callback: the count must not move afterwards. */
    unsigned settled = c.cycles;
    usleep(200 * 1000);
    TPW_ASSERT_EQ(c.cycles, settled);

    /* And the stream is restartable rather than spent. */
    TPW_ASSERT_EQ(tpw_stream_start(stream), TPW_OK);
    usleep(200 * 1000);
    TPW_ASSERT(c.cycles > settled);

    /* Drains, then stops like a plain stop: quiet and restartable. */
    TPW_ASSERT_EQ(tpw_stream_stop(stream, true), TPW_OK);
    unsigned drained_at = c.cycles;
    usleep(200 * 1000);
    TPW_ASSERT_EQ(c.cycles, drained_at);
    TPW_ASSERT_EQ(tpw_stream_start(stream), TPW_OK);
    usleep(200 * 1000);
    TPW_ASSERT(c.cycles > drained_at);

    tpw_stream_stop(stream, false);
    tpw_stream_destroy(stream);
    printf("test_stream_playback_hw: passed\n");
    return 0;
}
