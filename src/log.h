#ifndef TTYD_CLIENT_LOG_H
#define TTYD_CLIENT_LOG_H

#include <stdbool.h>

/* Log levels, intentionally matching the numeric values understood by
 * `ttyd --debug` (libwebsockets log level bits) so that the two tools can be
 * configured with the same numbers. */
#define TC_LLL_ERR 1
#define TC_LLL_WARN 2
#define TC_LLL_NOTICE 4
#define TC_LLL_INFO 8
#define TC_LLL_DEBUG 16
#define TC_LLL_PARSER 32
#define TC_LLL_HEADER 64
#define TC_LLL_EXT 128
#define TC_LLL_CLIENT 256
#define TC_LLL_LATENCY 512
#define TC_LLL_TIMESTAMP 1024

#define TC_DEFAULT_LOG_LEVEL (TC_LLL_ERR | TC_LLL_WARN | TC_LLL_NOTICE)

/* When enabled, every newline is written as CRLF (useful while the terminal
 * is in raw mode, where the kernel no longer translates LF) */
void log_set_crlf(bool enabled);

/* Set the log level bitmask, see TC_LLL_* */
void log_set_level(int level);

/* Get the current log level bitmask */
int log_get_level(void);

/* Whether the given level is enabled */
bool log_enabled(int level);

/* Log with an explicit level, prints to stderr */
void log_msg(int level, const char *fmt, ...);

/* Convenience macros */
#define log_err(...) log_msg(TC_LLL_ERR, __VA_ARGS__)
#define log_warn(...) log_msg(TC_LLL_WARN, __VA_ARGS__)
#define log_notice(...) log_msg(TC_LLL_NOTICE, __VA_ARGS__)
#define log_info(...) log_msg(TC_LLL_INFO, __VA_ARGS__)
#define log_debug(...) log_msg(TC_LLL_DEBUG, __VA_ARGS__)

#endif  // TTYD_CLIENT_LOG_H
