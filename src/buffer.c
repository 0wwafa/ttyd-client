#include "buffer.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "utils.h"

#define BUFFER_MIN_CAP 64

void buffer_init(buffer_t *buf) {
  buf->data = NULL;
  buf->len = 0;
  buf->cap = 0;
}

void buffer_free(buffer_t *buf) {
  if (buf == NULL) return;
  free(buf->data);
  buf->data = NULL;
  buf->len = 0;
  buf->cap = 0;
}

void buffer_reserve(buffer_t *buf, size_t extra) {
  size_t need = buf->len + extra + 1; /* +1 for the NUL terminator */
  if (need <= buf->cap) return;

  size_t cap = buf->cap ? buf->cap : BUFFER_MIN_CAP;
  while (cap < need) cap *= 2;
  buf->data = xrealloc(buf->data, cap);
  buf->cap = cap;
}

void buffer_append(buffer_t *buf, const void *data, size_t len) {
  if (len == 0) return;
  buffer_reserve(buf, len);
  memcpy(buf->data + buf->len, data, len);
  buf->len += len;
  buf->data[buf->len] = '\0';
}

void buffer_append_str(buffer_t *buf, const char *str) {
  if (str != NULL) buffer_append(buf, str, strlen(str));
}

void buffer_append_char(buffer_t *buf, char c) {
  buffer_reserve(buf, 1);
  buf->data[buf->len++] = c;
  buf->data[buf->len] = '\0';
}

void buffer_appendf(buffer_t *buf, const char *fmt, ...) {
  va_list ap, ap_copy;
  va_start(ap, fmt);
  va_copy(ap_copy, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0) {
    va_end(ap_copy);
    return;
  }
  buffer_reserve(buf, (size_t)n);
  vsnprintf(buf->data + buf->len, (size_t)n + 1, fmt, ap_copy);
  va_end(ap_copy);
  buf->len += (size_t)n;
}

void buffer_consume(buffer_t *buf, size_t len) {
  if (len >= buf->len) {
    buffer_reset(buf);
    return;
  }
  memmove(buf->data, buf->data + len, buf->len - len);
  buf->len -= len;
  buf->data[buf->len] = '\0';
}

void buffer_reset(buffer_t *buf) {
  buf->len = 0;
  if (buf->data != NULL) buf->data[0] = '\0';
}

char *buffer_detach(buffer_t *buf) {
  char *data = buf->data;
  if (data == NULL) data = xstrdup("");
  buf->data = NULL;
  buf->len = 0;
  buf->cap = 0;
  return data;
}
