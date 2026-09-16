/* SPDX-License-Identifier: MIT */

#include <getopt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tpw/tpw_stream.h"

typedef enum {
    RECORD_TYPE_AUDIO,
    RECORD_TYPE_VIDEO
} record_type;

typedef enum {
    FORMAT_UNSET = -1,
    FORMAT_WAV,
    FORMAT_PCM,
    FORMAT_RAW,
    FORMAT_Y4M
} record_format;

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

struct record_ctx {
    FILE* file;
    record_format format;
    uint32_t bytes_written;
};

static void on_data(tpw_stream_h stream, const tpw_stream_buffer* buf, void* user_data)
{
    (void)stream;
    struct record_ctx* ctx = user_data;
    if (ctx->format == FORMAT_Y4M)
        fwrite("FRAME\n", 1, 6, ctx->file);
    if (fwrite(buf->data, 1, buf->size, ctx->file) == buf->size)
        ctx->bytes_written += (uint32_t)buf->size;
}

static void on_error(tpw_stream_h stream, int error_code, void* user_data)
{
    (void)stream;
    (void)user_data;
    fprintf(stderr, "tpw_record: source lost (error %d)\n", error_code);
    g_running = 0;
}

static void put_le16(FILE* f, uint16_t v)
{
    unsigned char b[2] = { (unsigned char)(v & 0xFF), (unsigned char)((v >> 8) & 0xFF) };
    fwrite(b, 1, sizeof(b), f);
}

static void put_le32(FILE* f, uint32_t v)
{
    unsigned char b[4] = {
        (unsigned char)(v & 0xFF), (unsigned char)((v >> 8) & 0xFF),
        (unsigned char)((v >> 16) & 0xFF), (unsigned char)((v >> 24) & 0xFF)
    };
    fwrite(b, 1, sizeof(b), f);
}

/* Maps --bits's value to the tpw_audio_config.format string tpw_stream
 * expects, the on-disk sample width, and the WAV fmt-chunk tag (1 = PCM,
 * 3 = IEEE float). Only these four since they're exactly what
 * tpw_stream accepts (see src/tpw_spa_format.c's sample-format table). */
static const struct {
    const char* arg;
    const char* tpw_format;
    int bytes_per_sample;
    uint16_t wav_tag;
} audio_bit_depths[] = {
    { "16", "S16", 2, 1 },
    { "24", "S24", 3, 1 },
    { "32", "S32", 4, 1 },
    { "f32", "F32", 4, 3 },
};

/* Writes a 44-byte canonical WAV header at the current file position:
 * once as a zero-size placeholder before capture starts, and again
 * (seeked back to 0) to patch in the final size once it's known. */
static void write_wav_header(FILE* f, int sample_rate, int channels, int bytes_per_sample, uint16_t wav_tag,
                              uint32_t data_size)
{
    uint32_t byte_rate = (uint32_t)sample_rate * channels * bytes_per_sample;
    uint16_t block_align = (uint16_t)(channels * bytes_per_sample);
    uint16_t bits_per_sample = (uint16_t)(bytes_per_sample * 8);

    fwrite("RIFF", 1, 4, f);
    put_le32(f, 36 + data_size);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    put_le32(f, 16); /* fmt chunk size */
    put_le16(f, wav_tag);
    put_le16(f, (uint16_t)channels);
    put_le32(f, (uint32_t)sample_rate);
    put_le32(f, byte_rate);
    put_le16(f, block_align);
    put_le16(f, bits_per_sample);
    fwrite("data", 1, 4, f);
    put_le32(f, data_size);
}

/* Writes a YUV4MPEG2 stream header. Only correct for planar 4:2:0
 * (pixel_format "I420") — the other tpw pixel formats (RGB/YUYV/NV12/NV21/
 * MJPG) don't map onto Y4M's colorspace tags, so callers must reject those
 * before reaching here. fps must be a fixed, known value: Y4M has no
 * way to express "auto-negotiated". */
static void write_y4m_header(FILE* f, int width, int height, int fps)
{
    fprintf(f, "YUV4MPEG2 W%d H%d F%d:1 Ip A0:0 C420jpeg\n", width, height, fps);
}

static void print_usage(const char* prog)
{
    fprintf(stderr,
        "usage: %s -o <path> [-t audio|video] [-f <format>] [-d seconds]\n"
        "                 [--device name-or-serial]\n"
        "\n"
        "  -o, --output <path>       output file (required)\n"
        "  -t, --type audio|video    what to capture (default: audio)\n"
        "  -d, --duration <seconds>  stop after N seconds (default: run until Ctrl+C)\n"
        "      --device <name>       capture node name or serial instead of the\n"
        "                             default source (see `wpctl status`)\n"
        "\n"
        "  audio (-t audio):\n"
        "  -f, --format wav|pcm      container format (default: wav)\n"
        "      --sample-rate <hz>    capture sample rate (default: 48000)\n"
        "      --channels <n>        capture channel count (default: 2)\n"
        "      --bits 16|24|32|f32   sample format: signed int bit depth, or f32\n"
        "                            for 32-bit float (default: 16)\n"
        "\n"
        "  video (-t video):\n"
        "  -f, --format raw|y4m      container format (default: raw)\n"
        "                            y4m requires --pixel-format I420 and an\n"
        "                            explicit --fps\n"
        "      --width <px>          frame width (default: 640)\n"
        "      --height <px>         frame height (default: 480)\n"
        "      --pixel-format <fmt>  RGB, YUYV, NV12, NV21, I420, or MJPG (default: I420)\n"
        "      --fps <n>             frame rate; 0 lets the source pick it (default: 0)\n"
        "\n"
        "  -h, --help                show this help\n",
        prog);
}

int main(int argc, char** argv)
{
    const char* output = NULL;
    const char* device = NULL;
    record_type type = RECORD_TYPE_AUDIO;
    record_format format = FORMAT_UNSET;
    int duration = 0;
    int sample_rate = 48000;
    int channels = 2;
    int width = 640;
    int height = 480;
    int fps = 0;
    const char* pixel_format = "I420";
    const char* audio_format = NULL; /* tpw_audio_config.format; NULL = library default "S16" */
    int bytes_per_sample = 2;
    uint16_t wav_tag = 1; /* WAVE_FORMAT_PCM */

    static const struct option long_options[] = {
        { "output", required_argument, NULL, 'o' },
        { "type", required_argument, NULL, 't' },
        { "format", required_argument, NULL, 'f' },
        { "duration", required_argument, NULL, 'd' },
        { "device", required_argument, NULL, 1 },
        { "sample-rate", required_argument, NULL, 2 },
        { "channels", required_argument, NULL, 3 },
        { "width", required_argument, NULL, 4 },
        { "height", required_argument, NULL, 5 },
        { "pixel-format", required_argument, NULL, 6 },
        { "fps", required_argument, NULL, 7 },
        { "bits", required_argument, NULL, 8 },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "o:t:f:d:h", long_options, NULL)) != -1) {
        switch (opt) {
        case 'o':
            output = optarg;
            break;
        case 't':
            if (strcmp(optarg, "audio") == 0) {
                type = RECORD_TYPE_AUDIO;
            } else if (strcmp(optarg, "video") == 0) {
                type = RECORD_TYPE_VIDEO;
            } else {
                fprintf(stderr, "tpw_record: unknown type '%s' (expected audio or video)\n", optarg);
                return 1;
            }
            break;
        case 'f':
            if (strcmp(optarg, "wav") == 0) {
                format = FORMAT_WAV;
            } else if (strcmp(optarg, "pcm") == 0) {
                format = FORMAT_PCM;
            } else if (strcmp(optarg, "raw") == 0) {
                format = FORMAT_RAW;
            } else if (strcmp(optarg, "y4m") == 0) {
                format = FORMAT_Y4M;
            } else {
                fprintf(stderr, "tpw_record: unknown format '%s'\n", optarg);
                return 1;
            }
            break;
        case 'd':
            duration = atoi(optarg);
            if (duration <= 0) {
                fprintf(stderr, "tpw_record: --duration must be a positive integer\n");
                return 1;
            }
            break;
        case 1:
            device = optarg;
            break;
        case 2:
            sample_rate = atoi(optarg);
            break;
        case 3:
            channels = atoi(optarg);
            break;
        case 4:
            width = atoi(optarg);
            break;
        case 5:
            height = atoi(optarg);
            break;
        case 6:
            pixel_format = optarg;
            break;
        case 7:
            fps = atoi(optarg);
            break;
        case 8: {
            const size_t n = sizeof(audio_bit_depths) / sizeof(audio_bit_depths[0]);
            size_t i;
            for (i = 0; i < n; i++) {
                if (strcmp(optarg, audio_bit_depths[i].arg) == 0)
                    break;
            }
            if (i == n) {
                fprintf(stderr, "tpw_record: --bits must be 16, 24, 32, or f32\n");
                return 1;
            }
            audio_format = audio_bit_depths[i].tpw_format;
            bytes_per_sample = audio_bit_depths[i].bytes_per_sample;
            wav_tag = audio_bit_depths[i].wav_tag;
            break;
        }
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!output) {
        fprintf(stderr, "tpw_record: -o/--output is required\n\n");
        print_usage(argv[0]);
        return 1;
    }

    if (format == FORMAT_UNSET)
        format = (type == RECORD_TYPE_VIDEO) ? FORMAT_RAW : FORMAT_WAV;

    if (type == RECORD_TYPE_AUDIO && (format == FORMAT_RAW || format == FORMAT_Y4M)) {
        fprintf(stderr, "tpw_record: --format %s is video-only; audio uses wav or pcm\n",
            format == FORMAT_RAW ? "raw" : "y4m");
        return 1;
    }
    if (type == RECORD_TYPE_VIDEO && (format == FORMAT_WAV || format == FORMAT_PCM)) {
        fprintf(stderr, "tpw_record: --format %s is audio-only; video uses raw or y4m\n",
            format == FORMAT_WAV ? "wav" : "pcm");
        return 1;
    }
    if (format == FORMAT_Y4M && strcmp(pixel_format, "I420") != 0) {
        fprintf(stderr, "tpw_record: --format y4m requires --pixel-format I420 (got '%s')\n", pixel_format);
        return 1;
    }
    if (format == FORMAT_Y4M && fps <= 0) {
        fprintf(stderr, "tpw_record: --format y4m requires an explicit --fps (auto-negotiated rate can't be written to the header)\n");
        return 1;
    }

    struct record_ctx ctx = { .file = fopen(output, "wb"), .format = format, .bytes_written = 0 };
    if (!ctx.file) {
        fprintf(stderr, "tpw_record: failed to open '%s' for writing\n", output);
        return 1;
    }

    if (format == FORMAT_WAV)
        write_wav_header(ctx.file, sample_rate, channels, bytes_per_sample, wav_tag, 0); /* placeholder, patched at exit */
    else if (format == FORMAT_Y4M)
        write_y4m_header(ctx.file, width, height, fps);

    signal(SIGINT, on_signal);
    if (duration > 0) {
        signal(SIGALRM, on_signal);
        alarm((unsigned)duration);
    }

    tpw_stream_type stream_type = (type == RECORD_TYPE_VIDEO) ? TPW_STREAM_TYPE_VIDEO : TPW_STREAM_TYPE_AUDIO;
    tpw_stream_h stream = tpw_stream_create(stream_type, on_data, &ctx);
    if (!stream) {
        fprintf(stderr, "tpw_record: failed to create %s stream (is PipeWire running?)\n",
            type == RECORD_TYPE_VIDEO ? "video" : "audio");
        fclose(ctx.file);
        return 1;
    }

    tpw_stream_set_error_cb(stream, on_error);

    if (device && tpw_stream_set_target(stream, device) != TPW_OK) {
        fprintf(stderr, "tpw_record: failed to set target device\n");
        tpw_stream_destroy(stream);
        fclose(ctx.file);
        return 1;
    }

    int config_res;
    if (type == RECORD_TYPE_VIDEO) {
        tpw_video_config cfg = { .width = width, .height = height, .pixel_format = pixel_format, .fps = fps };
        config_res = tpw_stream_set_video_config(stream, &cfg);
    } else {
        tpw_audio_config cfg = { .sample_rate = sample_rate, .channels = channels, .format = audio_format };
        config_res = tpw_stream_set_audio_config(stream, &cfg);
    }
    if (config_res != TPW_OK) {
        fprintf(stderr, "tpw_record: failed to set %s format\n", type == RECORD_TYPE_VIDEO ? "video" : "audio");
        tpw_stream_destroy(stream);
        fclose(ctx.file);
        return 1;
    }

    if (tpw_stream_start(stream) != TPW_OK) {
        fprintf(stderr, "tpw_record: failed to start %s stream\n", type == RECORD_TYPE_VIDEO ? "video" : "audio");
        tpw_stream_destroy(stream);
        fclose(ctx.file);
        return 1;
    }

    if (type == RECORD_TYPE_VIDEO) {
        fprintf(stderr, "tpw_record: recording to '%s' (%s, %dx%d %s%s%s), press Ctrl+C to stop...\n",
            output, format == FORMAT_Y4M ? "y4m" : "raw", width, height, pixel_format,
            device ? ", device=" : "", device ? device : "");
    } else {
        fprintf(stderr, "tpw_record: recording to '%s' (%s, %dHz, %dch, %s%s%s), press Ctrl+C to stop...\n",
            output, format == FORMAT_WAV ? "wav" : "pcm", sample_rate, channels,
            audio_format ? audio_format : "S16",
            device ? ", device=" : "", device ? device : "");
    }
    while (g_running)
        sleep(1);

    tpw_stream_stop(stream, false);
    tpw_stream_destroy(stream);

    if (format == FORMAT_WAV) {
        fseek(ctx.file, 0, SEEK_SET);
        write_wav_header(ctx.file, sample_rate, channels, bytes_per_sample, wav_tag, ctx.bytes_written);
    }
    fclose(ctx.file);

    fprintf(stderr, "tpw_record: wrote %u bytes to '%s'\n", ctx.bytes_written, output);
    return 0;
}
