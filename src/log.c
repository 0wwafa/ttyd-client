#include "log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

static int log_level = TC_DEFAULT_LOG_LEVEL;
static bool log_crlf = false;

void log_set_crlf(bool enabled) { log_crlf = enabled; }

void log_set_level(int level) { log_level = level; }

int log_get_level(void) { return log_level; }

bool log_enabled(int level) { return (log_level & level) != 0; }

static const char *level_prefix(int level) {
  switch (level) {
    case TC_LLL_ERR:
      return "E";
    case TC_LLL_WARN:
      return "W";
    case TC_LLL_NOTICE:
      return "N";
    case TC_LLL_INFO:
      return "I";
    case TC_LLL_DEBUG:
    default:
      return "D";
  }
}

void log_msg(int level, const char *fmt, ...) {
  if (!log_enabled(level)) return;

  char stamp[32] = "";
  if (log_enabled(TC_LLL_TIMESTAMP)) {
    time_t t = time(NULL);
    struct tm tm_buf;
#if defined(_WIN32)
    struct tm *tm_ptr = localtime(&t);
    if (tm_ptr != NULL) tm_buf = *tm_ptr;
#else
    localtime_r(&t, &tm_buf);
#endif
    strftime(stamp, sizeof(stamp), "[%Y/%m/%d %H:%M:%S] ", &tm_buf);
  }

  char message[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(message, sizeof(message), fmt, ap);
  va_end(ap);

  fprintf(stderr, "%s%s: ", stamp, level_prefix(level));
  if (!log_crlf) {
    fputs(message, stderr);
  } else {
    for (const char *p = message; *p; p++) {
      if (*p == '\n' && (p == message || p[-1] != '\r')) fputc('\r', stderr);
      fputc(*p, stderr);
    }
  }
  fflush(stderr);
}
