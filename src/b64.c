#include "b64.h"

#include <string.h>

#include "utils.h"

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_value(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

size_t base64_encode(const void *in, size_t len, char *out, size_t out_len) {
  const unsigned char *src = (const unsigned char *)in;
  size_t need = 4 * ((len + 2) / 3);
  if (out == NULL || out_len < need + 1) return 0;

  size_t i = 0, o = 0;
  while (i + 2 < len) {
    unsigned int v = ((unsigned int)src[i] << 16) | ((unsigned int)src[i + 1] << 8) | src[i + 2];
    out[o++] = b64_table[(v >> 18) & 0x3f];
    out[o++] = b64_table[(v >> 12) & 0x3f];
    out[o++] = b64_table[(v >> 6) & 0x3f];
    out[o++] = b64_table[v & 0x3f];
    i += 3;
  }
  if (i < len) {
    unsigned int v = (unsigned int)src[i] << 16;
    bool two = (i + 1) < len;
    if (two) v |= (unsigned int)src[i + 1] << 8;
    out[o++] = b64_table[(v >> 18) & 0x3f];
    out[o++] = b64_table[(v >> 12) & 0x3f];
    out[o++] = two ? b64_table[(v >> 6) & 0x3f] : '=';
    out[o++] = '=';
  }
  out[o] = '\0';
  return o;
}

char *base64_encode_alloc(const void *in, size_t len) {
  size_t size = 4 * ((len + 2) / 3) + 1;
  char *buf = xmalloc(size);
  base64_encode(in, len, buf, size);
  return buf;
}

size_t base64_decode(const char *in, size_t in_len, unsigned char *out, size_t out_len) {
  size_t o = 0;
  unsigned int acc = 0;
  int bits = 0;

  for (size_t i = 0; i < in_len; i++) {
    unsigned char c = (unsigned char)in[i];
    if (c == '=' || c == '\r' || c == '\n') break;
    int v = b64_value(c);
    if (v < 0) return (size_t)-1;
    acc = (acc << 6) | (unsigned int)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (o >= out_len) return (size_t)-1;
      out[o++] = (unsigned char)((acc >> bits) & 0xff);
    }
  }
  return o;
}
