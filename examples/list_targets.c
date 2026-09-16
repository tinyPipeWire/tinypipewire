/* SPDX-License-Identifier: MIT */

#include <stdbool.h>
#include <stdio.h>

#include "tpw/tpw_stream.h"

static void on_data(tpw_stream_h stream, const tpw_stream_buffer* buf, void* user_data)
{
    (void)stream;
    (void)buf;
    (void)user_data;
}

static void on_fill(tpw_stream_h stream, tpw_stream_playback_buffer* buf, void* user_data)
{
    (void)stream;
    (void)user_data;
    buf->size = 0;
}

/* Video sources also report what they deliver. There is no audio equivalent:
 * a stream converts audio, so a device's own list would describe nothing. */
static void print_formats(tpw_stream_h stream, const char* target)
{
    tpw_video_format_info fmts[64];
    size_t n = 0;
    int res = tpw_stream_get_target_video_formats(stream, target, fmts, 64, &n);
    if (res != TPW_OK) {
        printf("      (could not read formats: error %d; in use elsewhere?)\n", res);
        return;
    }
    if (n == 0) {
        printf("      (the device names no format this library knows)\n");
        return;
    }

    for (size_t i = 0; i < n && i < 64; i++) {
        const tpw_video_format_info* f = &fmts[i];
        printf("      %-5s %dx%d", f->pixel_format, f->width, f->height);
        if (f->width_max != f->width || f->height_max != f->height)
            printf("..%dx%d", f->width_max, f->height_max);

        for (size_t r = 0; r < f->n_fps; r++)
            printf("%s%d", r == 0 ? " @" : ",", f->fps[r]);
        printf("\n");
    }
    if (n > 64)
        printf("      ... (%zu formats)\n", n);
}

static void print_targets(const char* label, tpw_stream_h stream, bool with_formats)
{
    if (!stream) {
        fprintf(stderr, "%s: failed to create stream (is PipeWire running?)\n", label);
        return;
    }

    tpw_target_info targets[32];
    size_t n = 0;
    int res = tpw_stream_get_target_list(stream, targets, 32, &n);
    if (res != TPW_OK) {
        fprintf(stderr, "%s: could not list targets (error %d)\n", label, res);
        tpw_stream_destroy(stream);
        return;
    }

    printf("%s: %zu target(s)%s\n", label, n, n > 32 ? " (showing first 32)" : "");
    for (size_t i = 0; i < n && i < 32; i++) {
        printf("  %s (serial %s) - %s\n", targets[i].name, targets[i].serial,
               targets[i].description[0] ? targets[i].description : "(no description)");
        if (with_formats)
            print_formats(stream, targets[i].name);
    }

    tpw_stream_destroy(stream);
}

int main(void)
{
    print_targets("Audio/Source", tpw_stream_create(TPW_STREAM_TYPE_AUDIO, on_data, NULL), false);
    print_targets("Video/Source", tpw_stream_create(TPW_STREAM_TYPE_VIDEO, on_data, NULL), true);
    print_targets("Audio/Sink", tpw_stream_create_playback(on_fill, NULL), false);
    return 0;
}
