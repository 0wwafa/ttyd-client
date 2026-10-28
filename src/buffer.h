#ifndef TTYD_CLIENT_BUFFER_H
#define TTYD_CLIENT_BUFFER_H

#include <stdbool.h>
#include <stddef.h>

/* A simple growable byte buffer. Also usable as a string builder,
 * `buf->data` is always NUL terminated when the buffer was created with
 * buffer_init(), which makes it safe to hand to string functions. */
typedef struct {
  char *data;
  size_t len;
  size_t cap;
} buffer_t;

void buffer_init(buffer_t *buf);
void buffer_free(buffer_t *buf);
void buffer_reserve(buffer_t *buf, size_t extra);
void buffer_append(buffer_t *buf, const void *data, size_t len);
void buffer_append_str(buffer_t *buf, const char *str);
void buffer_append_char(buffer_t *buf, char c);
void buffer_appendf(buffer_t *buf, const char *fmt, ...);
/* Remove `len` bytes from the front of the buffer */
void buffer_consume(buffer_t *buf, size_t len);
/* Empty the buffer, keeping the allocation */
void buffer_reset(buffer_t *buf);
/* Detach the data pointer (caller owns it), the buffer is reset */
char *buffer_detach(buffer_t *buf);

#endif  // TTYD_CLIENT_BUFFER_H
