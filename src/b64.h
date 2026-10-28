#ifndef TTYD_CLIENT_B64_H
#define TTYD_CLIENT_B64_H

#include <stdbool.h>
#include <stddef.h>

/* Base64 encode `len` bytes of `in` into `out` (NUL terminated).
 * `out_len` must be at least 4 * ((len + 2) / 3) + 1 bytes.
 * Returns the number of characters written (excluding the NUL terminator). */
size_t base64_encode(const void *in, size_t len, char *out, size_t out_len);

/* Allocate a NUL terminated base64 string for `in` */
char *base64_encode_alloc(const void *in, size_t len);

/* Base64 decode `in_len` characters of `in` into `out`.
 * Stops at the first invalid character or padding.
 * Returns the number of bytes written, or (size_t)-1 on error. */
size_t base64_decode(const char *in, size_t in_len, unsigned char *out, size_t out_len);

#endif  // TTYD_CLIENT_B64_H
