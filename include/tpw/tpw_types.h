/* SPDX-License-Identifier: MIT */

/**
 * @file tpw_types.h
 * @brief The data types tpw_stream and tpw_filter share: what a stream or
 *        port carries, how a call failed, and the format descriptions.
 */

#ifndef TPW_TYPES_H
#define TPW_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Classifies a stream, or a tpw_filter_* port, by the kind of data
 *        it carries.
 *
 * Audio and video are media; a signal and an event are not, which is why
 * this is a data type. SIGNAL and EVENT are filter-port-only:
 * tpw_stream_create() only accepts AUDIO/VIDEO and rejects the other two.
 */
typedef enum {
    TPW_DATA_AUDIO  = 0, /**< Raw audio samples. */
    TPW_DATA_VIDEO  = 1, /**< Raw video frames. */
    TPW_DATA_SIGNAL = 2, /**< One 32-bit float per frame, e.g. a sensor reading; filter ports only, see tpw_filter.h. */
    TPW_DATA_EVENT  = 3  /**< Discrete timestamped items such as MIDI or a property; filter ports only, see tpw_filter.h. */
} tpw_data_type;

/** @brief Library error codes. Negative values only; 0 is success. */
typedef enum {
    TPW_OK                     = 0,  /**< Success. */
    TPW_ERR_INVALID_ARG        = -1, /**< A NULL/out-of-range argument, or a call invalid for the object's current state or routing mode. */
    TPW_ERR_CONNECT_FAILED     = -2, /**< Connecting to PipeWire, or asking it to create a stream, link or query, failed. */
    TPW_ERR_INVALID_FORMAT     = -3, /**< An unrecognized pixel/sample format string, or an out-of-range dimension/rate. */
    TPW_ERR_NOT_CONFIGURED     = -4, /**< Called before a required prior step, e.g. start() before a format was set, link() before start(), or unlink() with nothing linked. */
    TPW_ERR_SOURCE_UNAVAILABLE = -5, /**< The connected source disappeared, or could not provide the requested memory type (see tpw_stream_error_cb). */
    TPW_ERR_IN_CALLBACK        = -6, /**< Called from inside one of the object's own callbacks, where the call cannot run; call it again after the callback returns. */
    TPW_ERR_NOT_FOUND          = -7, /**< No node in the graph matches the target name or serial; it may have gone away or never existed. */
    TPW_ERR_TIMEOUT            = -8, /**< PipeWire did not answer, or a link did not negotiate, within the library's time limit; the same call may succeed if retried. */
    TPW_ERR_NO_MEMORY          = -9  /**< A memory allocation failed. */
} tpw_error;

/**
 * @brief Audio capture configuration passed to
 *        tpw_stream_set_audio_config() and tpw_filter_add_audio_port().
 */
typedef struct {
    int sample_rate;    /**< Hz, e.g. 48000. */
    int channels;       /**< Channel count, e.g. 2. */
    const char* format; /**< "U8", "S16", "S24", "S24_32", "S32", or "F32"; NULL defaults to "S16". */
} tpw_audio_config;

/** @brief Video capture configuration passed to tpw_stream_set_video_config(). */
typedef struct {
    int width;                /**< Frame width in pixels. */
    int height;               /**< Frame height in pixels. */
    const char* pixel_format; /**< "RGB", "YUYV", "NV12", "NV21", "I420", "MJPG", or "H264". MJPG and H264 are compressed: each delivered frame's size varies, and DMABUF delivery is not available for them. */
    int fps;                  /**< Frames per second; 0 negotiates automatically. */
} tpw_video_config;

/**
 * @brief Selects a video port/stream's buffer memory.
 *
 * AUTO is the default (graph-selected, normally CPU-mapped). DMABUF
 * negotiates file descriptors instead of a CPU buffer.
 */
typedef enum {
    TPW_PORT_MEMORY_AUTO   = 0, /**< Graph-selected, normally CPU-mapped. */
    TPW_PORT_MEMORY_DMABUF = 1  /**< Negotiate DMABUF file descriptors. */
} tpw_port_memory;

/**
 * @brief One plane of a DMABUF-delivered frame.
 *
 * `fd` is borrowed (import-only, not owned): valid only for the callback
 * that received it, do not close it.
 */
typedef struct {
    int      fd;     /**< Borrowed DMABUF file descriptor; do not close. */
    uint32_t offset; /**< Byte offset to the plane within the dmabuf. */
    uint32_t stride; /**< Row stride in bytes. */
    uint32_t size;   /**< Valid bytes of this plane. */
} tpw_dmabuf_plane;

/**
 * @brief One pixel format and frame size a target can deliver.
 *
 * Every field goes straight into a tpw_video_config; a pixel format this
 * library cannot name is left out rather than reported as unusable. A
 * device taking a range of sizes reports its ends, so `width_max`/
 * `height_max` exceed `width`/`height` there and equal them otherwise.
 */
typedef struct {
    char   pixel_format[16]; /**< "RGB", "YUYV", "NV12", "NV21", "I420", "MJPG", or "H264", as tpw_video_config takes it. */
    int    width;            /**< Frame width in pixels, or the smallest one for a size range. */
    int    height;           /**< Frame height in pixels, or the smallest one for a size range. */
    int    width_max;        /**< Equal to `width` for a discrete size, the range's largest width otherwise. */
    int    height_max;       /**< Equal to `height` for a discrete size, the range's largest height otherwise. */
    int    fps[8];           /**< Frame rates at this size, highest first; whole frames per second, as tpw_video_config takes them. */
    size_t n_fps;            /**< Entries set in `fps`, never more than it holds; a device offering more keeps its fastest rates. */
} tpw_video_format_info;

#ifdef __cplusplus
}
#endif

#endif /* TPW_TYPES_H */
