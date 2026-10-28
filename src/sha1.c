/* SHA-1, implemented from the FIPS 180-1 specification.
 * Only used for the RFC 6455 "Sec-WebSocket-Accept" handshake value, where
 * SHA-1 is mandated by the protocol and carries no security requirements. */
#include "sha1.h"

#include <string.h>

#define ROL(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

static void sha1_transform(uint32_t state[5], const unsigned char buffer[64]) {
  uint32_t w[80];
  uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];

  for (int i = 0; i < 16; i++) {
    w[i] = ((uint32_t)buffer[i * 4] << 24) | ((uint32_t)buffer[i * 4 + 1] << 16) | ((uint32_t)buffer[i * 4 + 2] << 8) |
           (uint32_t)buffer[i * 4 + 3];
  }
  for (int i = 16; i < 80; i++) {
    w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  }

  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20) {
      f = (b & c) | ((~b) & d);
      k = 0x5a827999;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ed9eba1;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8f1bbcdc;
    } else {
      f = b ^ c ^ d;
      k = 0xca62c1d6;
    }
    uint32_t temp = ROL(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = ROL(b, 30);
    b = a;
    a = temp;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
}

void sha1_init(sha1_ctx *ctx) {
  ctx->state[0] = 0x67452301;
  ctx->state[1] = 0xefcdab89;
  ctx->state[2] = 0x98badcfe;
  ctx->state[3] = 0x10325476;
  ctx->state[4] = 0xc3d2e1f0;
  ctx->count = 0;
}

void sha1_update(sha1_ctx *ctx, const void *data, size_t len) {
  const unsigned char *p = (const unsigned char *)data;
  size_t used = (size_t)(ctx->count % 64);
  ctx->count += len;

  if (used > 0) {
    size_t need = 64 - used;
    if (len < need) {
      memcpy(ctx->buffer + used, p, len);
      return;
    }
    memcpy(ctx->buffer + used, p, need);
    sha1_transform(ctx->state, ctx->buffer);
    p += need;
    len -= need;
  }

  while (len >= 64) {
    sha1_transform(ctx->state, p);
    p += 64;
    len -= 64;
  }

  if (len > 0) memcpy(ctx->buffer, p, len);
}

void sha1_final(sha1_ctx *ctx, unsigned char digest[SHA1_DIGEST_SIZE]) {
  uint64_t bits = ctx->count * 8;
  size_t used = (size_t)(ctx->count % 64);
  unsigned char pad[72];
  size_t pad_len;

  pad[0] = 0x80;
  pad_len = (used < 56) ? (56 - used) : (120 - used);
  memset(pad + 1, 0, pad_len - 1);

  sha1_update(ctx, pad, pad_len);

  unsigned char len_buf[8];
  for (int i = 0; i < 8; i++) len_buf[i] = (unsigned char)(bits >> (56 - 8 * i));
  sha1_update(ctx, len_buf, sizeof(len_buf));

  for (int i = 0; i < 5; i++) {
    digest[i * 4] = (unsigned char)(ctx->state[i] >> 24);
    digest[i * 4 + 1] = (unsigned char)(ctx->state[i] >> 16);
    digest[i * 4 + 2] = (unsigned char)(ctx->state[i] >> 8);
    digest[i * 4 + 3] = (unsigned char)(ctx->state[i]);
  }
}

void sha1(const void *data, size_t len, unsigned char digest[SHA1_DIGEST_SIZE]) {
  sha1_ctx ctx;
  sha1_init(&ctx);
  sha1_update(&ctx, data, len);
  sha1_final(&ctx, digest);
}
