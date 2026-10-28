#include "utils.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "compat.h"

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#undef NSIG
#define NSIG 33
const char *sys_signame[NSIG] = {"zero", "HUP",   "INT",  "QUIT", "ILL",  "TRAP", "IOT",  "EMT",  "FPE",
                                 "KILL", "BUS",   "SEGV", "SYS",  "PIPE", "ALRM", "TERM", "URG",  "STOP",
                                 "TSTP", "CONT",  "CHLD", "TTIN", "TTOU", "IO",   "XCPU", "XFSZ", "VTALRM",
                                 "PROF", "WINCH", "PWR",  "USR1", "USR2", NULL};
#elif defined(__linux__) && !defined(__ANDROID__)
const char *sys_signame[NSIG] = {"zero", "HUP",   "INT",  "QUIT", "ILL",  "TRAP", "ABRT", "BUS",    "FPE",
                                 "KILL", "USR1",  "SEGV", "USR2", "PIPE", "ALRM", "TERM", "STKFLT", "CHLD",
                                 "CONT", "STOP",  "TSTP", "TTIN", "TTOU", "URG",  "XCPU", "XFSZ",   "VTALRM",
                                 "PROF", "WINCH", "IO",   "PWR",  "SYS",  NULL};
#else
/* Fallback table (macOS, FreeBSD, OpenBSD, ...). Values are advisory only:
 * they are used to print and to look up the names ttyd accepts. */
const char *sys_signame[NSIG] = {"zero", "HUP",   "INT",  "QUIT", "ILL",  "TRAP", "ABRT", "EMT",  "FPE",
                                 "KILL", "BUS",   "SEGV", "SYS",  "PIPE", "ALRM", "TERM", "URG",  "STOP",
                                 "TSTP", "CONT",  "CHLD", "TTIN", "TTOU", "IO",   "XCPU", "XFSZ", "VTALRM",
                                 "PROF", "WINCH", "PWR",  "USR1", "USR2", NULL};
#endif

void *xmalloc(size_t size) {
  if (size == 0) return NULL;
  void *p = malloc(size);
  if (!p) abort();
  return p;
}

void *xcalloc(size_t nmemb, size_t size) {
  if (nmemb == 0 || size == 0) return NULL;
  void *p = calloc(nmemb, size);
  if (!p) abort();
  return p;
}

void *xrealloc(void *p, size_t size) {
  if ((size == 0) && (p == NULL)) return NULL;
  p = realloc(p, size);
  if (!p) abort();
  return p;
}

char *xstrdup(const char *s) {
  if (s == NULL) return NULL;
  size_t len = strlen(s) + 1;
  char *p = xmalloc(len);
  memcpy(p, s, len);
  return p;
}

char *xstrndup(const char *s, size_t n) {
  if (s == NULL) return NULL;
  size_t len = strnlen(s, n);
  char *p = xmalloc(len + 1);
  memcpy(p, s, len);
  p[len] = '\0';
  return p;
}

char *uppercase(char *s) {
  char *p = s;
  while (*p) {
    *p = (char)toupper((int)(unsigned char)*p);
    p++;
  }
  return s;
}

char *lowercase(char *s) {
  char *p = s;
  while (*p) {
    *p = (char)tolower((int)(unsigned char)*p);
    p++;
  }
  return s;
}

bool endswith(const char *str, const char *suffix) {
  if (str == NULL || suffix == NULL) return false;
  size_t str_len = strlen(str);
  size_t suffix_len = strlen(suffix);
  return str_len >= suffix_len && !strcmp(str + (str_len - suffix_len), suffix);
}

bool startswith(const char *str, const char *prefix) {
  if (str == NULL || prefix == NULL) return false;
  return strncmp(str, prefix, strlen(prefix)) == 0;
}

bool str_eq_ignore_case(const char *a, const char *b) {
  if (a == NULL || b == NULL) return a == b;
  return strcasecmp(a, b) == 0;
}

char *str_trim(char *str) {
  if (str == NULL) return NULL;
  char *start = str;
  while (*start && isspace((unsigned char)*start)) start++;
  char *end = start + strlen(start);
  while (end > start && isspace((unsigned char)end[-1])) end--;
  *end = '\0';
  if (start != str) memmove(str, start, (size_t)(end - start) + 1);
  return str;
}

bool parse_long(const char *str, long *out) {
  if (str == NULL || *str == '\0') return false;
  char *endptr = NULL;
  errno = 0;
  long val = strtol(str, &endptr, 10);
  if (errno != 0 || endptr == str) return false;
  while (*endptr && isspace((unsigned char)*endptr)) endptr++;
  if (*endptr != '\0') return false;
  if (out) *out = val;
  return true;
}

int get_sig_name(int sig, char *buf, size_t len) {
  int n = snprintf(buf, len, "SIG%s", sig > 0 && sig < NSIG ? sys_signame[sig] : "unknown");
  uppercase(buf);
  return n;
}

int get_sig(const char *sig_name) {
  if (sig_name == NULL) return -1;
  for (int sig = 1; sig < NSIG; sig++) {
    const char *name = sys_signame[sig];
    if (name == NULL) continue;
    if (strcasecmp(name, sig_name) == 0) return sig;
    if (strncasecmp(sig_name, "SIG", 3) == 0 && strcasecmp(name, sig_name + 3) == 0) return sig;
  }
  long val = 0;
  if (parse_long(sig_name, &val) && val > 0 && val < NSIG) return (int)val;
  return -1;
}

uint64_t now_ms(void) {
#if defined(_WIN32)
  return (uint64_t)GetTickCount64();
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

void sleep_ms(unsigned int ms) {
#if defined(_WIN32)
  Sleep(ms);
#else
  struct timespec ts;
  ts.tv_sec = (time_t)(ms / 1000);
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    /* keep waiting for the remaining time */
  }
#endif
}

bool random_bytes(unsigned char *buf, size_t len) {
#if defined(_WIN32)
  /* The Windows system CSPRNG, used for the websocket masking keys. */
  return BCryptGenRandom(NULL, buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
  FILE *fp = fopen("/dev/urandom", "rb");
  if (fp != NULL) {
    size_t n = fread(buf, 1, len, fp);
    fclose(fp);
    if (n == len) return true;
  }
  /* fall back to a time seeded PRNG, only used for websocket masking keys */
  static bool seeded = false;
  if (!seeded) {
    srand((unsigned int)(time(NULL) ^ (uintptr_t)buf));
    seeded = true;
  }
  for (size_t i = 0; i < len; i++) buf[i] = (unsigned char)(rand() & 0xff);
  return true;
#endif
}

char *escape_for_terminal(const char *in, size_t len, char *out) {
  size_t n = 0;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)in[i];
    out[n++] = (c < 0x20 || c == 0x7f || c == 0x1b || c == 0x07 || c == 0x9c) ? '?' : (char)c;
  }
  out[n] = '\0';
  return out;
}
