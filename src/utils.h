#ifndef TTYD_CLIENT_UTILS_H
#define TTYD_CLIENT_UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* malloc with NULL check */
void *xmalloc(size_t size);

/* calloc with NULL check */
void *xcalloc(size_t nmemb, size_t size);

/* realloc with NULL check */
void *xrealloc(void *p, size_t size);

/* strdup with NULL check */
char *xstrdup(const char *s);

/* strndup with NULL check */
char *xstrndup(const char *s, size_t n);

/* Convert a string to upper case (in place) */
char *uppercase(char *s);

/* Convert a string to lower case (in place) */
char *lowercase(char *s);

/* Check whether str ends with suffix */
bool endswith(const char *str, const char *suffix);

/* Check whether str starts with prefix */
bool startswith(const char *str, const char *prefix);

/* Case insensitive string equality, NULL-safe */
bool str_eq_ignore_case(const char *a, const char *b);

/* Trim leading and trailing whitespace in place, returns str */
char *str_trim(char *str);

/* Parse a long integer, returns false on syntax error */
bool parse_long(const char *str, long *out);

/* Get human readable signal string, eg: SIGHUP */
int get_sig_name(int sig, char *buf, size_t len);

/* Get signal code from string like SIGHUP or HUP, returns -1 when unknown */
int get_sig(const char *sig_name);

/* Milliseconds since an arbitrary monotonic point */
uint64_t now_ms(void);

/* Sleep for the given number of milliseconds */
void sleep_ms(unsigned int ms);

/* Fill buf with cryptographically strong random bytes (best effort) */
bool random_bytes(unsigned char *buf, size_t len);

/* Escape a string for safe use inside a terminal escape sequence:
 * replaces control characters (< 0x20, 0x7f) and the escape introducers with '?'.
 * `out` must be at least len + 1 bytes. Returns out. */
char *escape_for_terminal(const char *in, size_t len, char *out);

#endif  // TTYD_CLIENT_UTILS_H
