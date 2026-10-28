#ifndef TTYD_CLIENT_SHA1_H
#define TTYD_CLIENT_SHA1_H

#include <stddef.h>
#include <stdint.h>

#define SHA1_DIGEST_SIZE 20

typedef struct {
  uint32_t state[5];
  uint64_t count;
  unsigned char buffer[64];
} sha1_ctx;

void sha1_init(sha1_ctx *ctx);
void sha1_update(sha1_ctx *ctx, const void *data, size_t len);
void sha1_final(sha1_ctx *ctx, unsigned char digest[SHA1_DIGEST_SIZE]);

/* One shot helper */
void sha1(const void *data, size_t len, unsigned char digest[SHA1_DIGEST_SIZE]);

#endif  // TTYD_CLIENT_SHA1_H
