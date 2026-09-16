/* SPDX-License-Identifier: MIT */

#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "tpw/tpw_stream.h"

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

static void on_data(tpw_stream_h stream, const tpw_stream_buffer* buf, void* user_data)
{
    (void)stream;
    (void)user_data;
    printf("audio: received %zu bytes (pts=%lld ns)\n", buf->size, (long long)buf->pts);
}

static void on_error(tpw_stream_h stream, int error_code, void* user_data)
{
    (void)stream;
    (void)user_data;
    fprintf(stderr, "audio: source lost (error %d)\n", error_code);
    g_running = 0;
}

int main(void)
{
    signal(SIGINT, on_signal);

    tpw_stream_h stream = tpw_stream_create(TPW_STREAM_TYPE_AUDIO, on_data, NULL);
    if (!stream) {
        fprintf(stderr, "failed to create audio stream (is PipeWire running?)\n");
        return 1;
    }

    tpw_stream_set_error_cb(stream, on_error);

    tpw_audio_config cfg = { .sample_rate = 48000, .channels = 2 };
    if (tpw_stream_set_audio_config(stream, &cfg) != TPW_OK) {
        fprintf(stderr, "failed to set audio format\n");
        tpw_stream_destroy(stream);
        return 1;
    }

    if (tpw_stream_start(stream) != TPW_OK) {
        fprintf(stderr, "failed to start audio stream\n");
        tpw_stream_destroy(stream);
        return 1;
    }

    printf("capturing audio, press Ctrl+C to stop...\n");
    while (g_running)
        sleep(1);

    tpw_stream_stop(stream, false);
    tpw_stream_destroy(stream);
    return 0;
}
