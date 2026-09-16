/* SPDX-License-Identifier: MIT */

#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "tpw/tpw_stream.h"

#define SAMPLE_RATE 48000
#define CHANNELS 2
#define TONE_HZ 440.0

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

/* Runs on the real-time thread: generate and return, nothing else. */
static void on_fill(tpw_stream_h stream, tpw_stream_playback_buffer* buf, void* user_data)
{
    (void)stream;
    static double phase;
    double* reported = user_data;

    size_t frame = sizeof(int16_t) * CHANNELS;
    size_t frames = buf->available / frame;
    int16_t* out = buf->data;

    for (size_t i = 0; i < frames; i++) {
        int16_t v = (int16_t)(sin(phase) * 0.2 * INT16_MAX);
        for (int c = 0; c < CHANNELS; c++)
            *out++ = v;
        phase += 2.0 * M_PI * TONE_HZ / SAMPLE_RATE;
        if (phase >= 2.0 * M_PI)
            phase -= 2.0 * M_PI;
    }

    buf->size = frames * frame;
    *reported = (double)buf->pts;
}

static void on_error(tpw_stream_h stream, int error_code, void* user_data)
{
    (void)stream;
    (void)user_data;
    fprintf(stderr, "playback: output device lost (error %d)\n", error_code);
    g_running = 0;
}

int main(int argc, char** argv)
{
    signal(SIGINT, on_signal);

    /* Shared with the callback only to show the presentation timestamp. */
    double last_pts = -1.0;

    tpw_stream_h stream = tpw_stream_create_playback(on_fill, &last_pts);
    if (!stream) {
        fprintf(stderr, "failed to create playback stream (is PipeWire running?)\n");
        return 1;
    }

    tpw_stream_set_error_cb(stream, on_error);

    if (argc > 1 && tpw_stream_set_target(stream, argv[1]) != TPW_OK) {
        fprintf(stderr, "failed to select output device '%s'\n", argv[1]);
        tpw_stream_destroy(stream);
        return 1;
    }

    tpw_audio_config cfg = { .sample_rate = SAMPLE_RATE, .channels = CHANNELS, .format = "S16" };
    if (tpw_stream_set_audio_config(stream, &cfg) != TPW_OK) {
        fprintf(stderr, "failed to set audio format\n");
        tpw_stream_destroy(stream);
        return 1;
    }

    if (tpw_stream_start(stream) != TPW_OK) {
        fprintf(stderr, "failed to start playback\n");
        tpw_stream_destroy(stream);
        return 1;
    }

    printf("playing a %g Hz tone to %s, press Ctrl+C to stop...\n", TONE_HZ,
           argc > 1 ? argv[1] : "the default output device");
    while (g_running) {
        sleep(1);
        printf("playback: next samples land at pts=%lld ns\n", (long long)last_pts);
    }

    tpw_stream_stop(stream, false);
    tpw_stream_destroy(stream);
    return 0;
}
