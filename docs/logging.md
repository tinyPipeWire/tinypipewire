# Logging

`include/tpw/tpw_log.h` lets an application redirect or filter the
library's internal diagnostic messages instead of only seeing raw
PipeWire stderr output:

```c
typedef enum { TPW_LOG_ERROR, TPW_LOG_WARNING, TPW_LOG_INFO, TPW_LOG_DEBUG, TPW_LOG_VERBOSE } tpw_log_level;
typedef void (*tpw_log_cb)(tpw_log_level level, const char* file, int line, const char* message, void* user_data);

void tpw_log_set_callback(tpw_log_cb callback, void* user_data);
void tpw_log_set_level(tpw_log_level level);
```

With no callback registered, messages are written to stderr, tagged
with the source file (basename) and line that logged them, mirroring
PipeWire's own log output. The minimum level defaults to
`TPW_LOG_WARNING`; call `tpw_log_set_level()` to see
`TPW_LOG_INFO`/`TPW_LOG_DEBUG`/`TPW_LOG_VERBOSE` messages too, or to
route everything through your own logger:

```c
void my_logger(tpw_log_level level, const char* file, int line, const char* message, void* user_data) {
    fprintf(stderr, "[myapp] %s:%d: %s\n", file, line, message);
}
tpw_log_set_callback(my_logger, NULL);
```

The callback runs on whichever thread logged the message, and some messages,
such as a playback callback overrunning its cycle, are logged from a stream's
or filter's real-time data thread. Keep the callback from blocking: if your
logger writes to a file or a socket, hand the message to another thread.

## See also

- [Streams](streams.md) — single-source capture and audio playback
- [Filters](filters.md) — combining several sources into one processed output
