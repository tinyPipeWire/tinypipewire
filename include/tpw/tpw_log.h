/* SPDX-License-Identifier: MIT */

#ifndef TPW_LOG_H
#define TPW_LOG_H

#include "tpw/tpw_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Severity of one log message, most to least severe. */
typedef enum {
    TPW_LOG_ERROR   = 0,
    TPW_LOG_WARNING = 1,
    TPW_LOG_INFO    = 2,
    TPW_LOG_DEBUG   = 3,
    TPW_LOG_VERBOSE = 4
} tpw_log_level;

/* Receives one already-formatted message from the library, tagged
 * with the source file (basename) and line that logged it, mirroring
 * PipeWire's own file/line-tagged log output. `file` and `message`
 * are valid only for the duration of this call. */
typedef void (*tpw_log_cb)(tpw_log_level level, const char* file, int line, const char* message, void* user_data);

/* Registers (or clears, with NULL) the process-wide log callback,
 * replacing whatever was set before. With no callback set, messages
 * are written to stderr instead. */
TPW_API void tpw_log_set_callback(tpw_log_cb callback, void* user_data);

/* Sets the minimum severity delivered to the callback (or stderr);
 * messages less severe than `level` are dropped before formatting.
 * Default: TPW_LOG_WARNING. */
TPW_API void tpw_log_set_level(tpw_log_level level);

#ifdef __cplusplus
}
#endif

#endif /* TPW_LOG_H */
