#ifndef TTYD_CLIENT_TLS_H
#define TTYD_CLIENT_TLS_H

#include <stdbool.h>
#include <stddef.h>

#include "net.h"

/* TLS support is always built in. */

typedef struct {
  bool verify;             /* verify the peer certificate (default: true) */
  const char *ca_file;     /* CA bundle override; NULL = platform trust store
                            * (Windows certificate store on Windows, OpenSSL
                            * default paths plus the conventional system CA
                            * bundle locations elsewhere) */
  const char *cert_file;   /* client certificate, NULL = none */
  const char *key_file;    /* client key, NULL = none */
  const char *server_name; /* SNI / verification name, NULL = use the url host */
  int timeout_ms;          /* handshake timeout */
} tls_options;

typedef struct tc_tls tc_tls;

/* Perform the TLS handshake over an already connected socket.
 * Returns NULL and fills `err` on failure. */
tc_tls *tc_tls_start(socket_t sock, const char *host, const tls_options *options, char *err, size_t err_len);

/* Read/write, return values follow net_read()/net_write() */
int tc_tls_read(tc_tls *tls, void *buf, size_t len);
int tc_tls_write(tc_tls *tls, const void *buf, size_t len);

/* Number of decrypted bytes already buffered inside the TLS layer */
int tc_tls_pending(tc_tls *tls);

/* Send close_notify and free the TLS state (the socket is left open) */
void tc_tls_free(tc_tls *tls);

#endif  // TTYD_CLIENT_TLS_H
