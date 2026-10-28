#ifndef TTYD_CLIENT_IO_H
#define TTYD_CLIENT_IO_H

#include <stdbool.h>
#include <stddef.h>

#include "net.h"
#include "tls.h"
#include "url.h"

/* An established byte stream: TCP, optionally tunnelled through an HTTP proxy
 * and optionally wrapped in TLS. */
typedef struct {
  socket_t fd;
  tc_tls *tls;
} tc_io;

typedef struct {
  bool enabled;
  char *host;
  int port;
  char *user; /* proxy authentication, optional */
  char *password;
} tc_proxy;

/* Open a stream to the url target, going through `proxy` when enabled.
 * Fills `err` and returns false on failure. */
bool io_open(tc_io *io, const tc_url *url, const tls_options *tls_opts, const tc_proxy *proxy, int timeout_ms,
             char *err, size_t err_len);

/* Read up to `len` bytes: >0 bytes read, TC_IO_EOF or TC_IO_ERROR */
int io_read(tc_io *io, void *buf, size_t len);

/* Write up to `len` bytes: >0 bytes written or TC_IO_ERROR */
int io_write(tc_io *io, const void *buf, size_t len);

/* Write the whole buffer, returns false on error */
bool io_write_all(tc_io *io, const void *buf, size_t len);

/* Wait until the stream is readable (also true when TLS already has data) */
bool io_wait_readable(tc_io *io, int timeout_ms);

/* Whether the TLS layer holds already decrypted bytes */
bool io_has_buffered(tc_io *io);

/* Close the stream, sending close_notify for TLS */
void io_close(tc_io *io);

#endif  // TTYD_CLIENT_IO_H
