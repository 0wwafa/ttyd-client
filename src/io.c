#include "io.h"

#include <stdio.h>
#include <string.h>

#include "b64.h"
#include "log.h"
#include "utils.h"

/* Send an HTTP CONNECT request over a plain socket and read the response head.
 * Returns true when the proxy answered "200". */
static bool proxy_connect(socket_t fd, const char *host, int port, const char *user, const char *password, char *err,
                          size_t err_len) {
  char target[512];
  snprintf(target, sizeof(target), "%s:%d", host, port);

  char request[1024];
  int n = snprintf(request, sizeof(request),
                   "CONNECT %s HTTP/1.1\r\n"
                   "Host: %s\r\n"
                   "User-Agent: ttyd-client/" TTYD_CLIENT_VERSION "\r\n",
                   target, target);
  if (user != NULL) {
    char cred[512];
    snprintf(cred, sizeof(cred), "%s:%s", user, password != NULL ? password : "");
    char *encoded = base64_encode_alloc(cred, strlen(cred));
    n += snprintf(request + n, sizeof(request) - (size_t)n, "Proxy-Authorization: Basic %s\r\n", encoded);
    free(encoded);
  }
  n += snprintf(request + n, sizeof(request) - (size_t)n, "\r\n");

  if (net_write(fd, request, (size_t)n) != n) {
    snprintf(err, err_len, "cannot send CONNECT request to proxy");
    return false;
  }

  /* Read the response head */
  char head[4096];
  size_t len = 0;
  while (len + 1 < sizeof(head)) {
    int rc = net_read(fd, head + len, sizeof(head) - len - 1);
    if (rc == TC_IO_ERROR) {
      snprintf(err, err_len, "proxy connection failed");
      return false;
    }
    if (rc == TC_IO_EOF) break;
    len += (size_t)rc;
    head[len] = '\0';
    if (strstr(head, "\r\n\r\n") != NULL) break;
  }

  int status = 0;
  if (sscanf(head, "HTTP/%*d.%*d %d", &status) != 1) {
    snprintf(err, err_len, "invalid response from proxy");
    return false;
  }
  if (status != 200) {
    snprintf(err, err_len, "proxy refused CONNECT to %s: HTTP %d", target, status);
    return false;
  }
  return true;
}

bool io_open(tc_io *io, const tc_url *url, const tls_options *tls_opts, const tc_proxy *proxy, int timeout_ms,
             char *err, size_t err_len) {
  memset(io, 0, sizeof(*io));
  io->fd = TC_INVALID_SOCKET;

  const char *connect_host = url->host;
  int connect_port = url->port;
  if (proxy != NULL && proxy->enabled) {
    connect_host = proxy->host;
    connect_port = proxy->port;
  }

  log_debug("connecting to %s:%d\n", connect_host, connect_port);
  io->fd = net_connect(connect_host, connect_port, timeout_ms, err, err_len);
  if (io->fd == TC_INVALID_SOCKET) return false;

  if (proxy != NULL && proxy->enabled) {
    log_debug("tunnelling through proxy %s:%d\n", proxy->host, proxy->port);
    if (!proxy_connect(io->fd, url->host, url->port, proxy->user, proxy->password, err, err_len)) {
      net_close(io->fd);
      io->fd = TC_INVALID_SOCKET;
      return false;
    }
  }

  if (url->tls) {
    if (!tc_tls_available()) {
      snprintf(err, err_len, "this build of ttyd-client does not support TLS, use a ws:// url instead");
      net_close(io->fd);
      io->fd = TC_INVALID_SOCKET;
      return false;
    }
    tls_options opts = *tls_opts;
    if (opts.timeout_ms <= 0) opts.timeout_ms = timeout_ms;
    io->tls = tc_tls_start(io->fd, url->host, &opts, err, err_len);
    if (io->tls == NULL) {
      net_close(io->fd);
      io->fd = TC_INVALID_SOCKET;
      return false;
    }
  }

  return true;
}

int io_read(tc_io *io, void *buf, size_t len) {
  if (io->tls != NULL) return tc_tls_read(io->tls, buf, len);
  return net_read(io->fd, buf, len);
}

int io_write(tc_io *io, const void *buf, size_t len) {
  if (io->tls != NULL) return tc_tls_write(io->tls, buf, len);
  return net_write(io->fd, buf, len);
}

bool io_write_all(tc_io *io, const void *buf, size_t len) {
  const char *p = (const char *)buf;
  size_t written = 0;
  while (written < len) {
    int n = io_write(io, p + written, len - written);
    if (n <= 0) return false;
    written += (size_t)n;
  }
  return true;
}

bool io_wait_readable(tc_io *io, int timeout_ms) {
  if (io_has_buffered(io)) return true;
  return net_wait_readable(io->fd, timeout_ms);
}

bool io_has_buffered(tc_io *io) { return io->tls != NULL && tc_tls_pending(io->tls) > 0; }

void io_close(tc_io *io) {
  if (io == NULL) return;
  if (io->tls != NULL) {
    tc_tls_free(io->tls);
    io->tls = NULL;
  }
  if (io->fd != TC_INVALID_SOCKET) {
    net_shutdown(io->fd);
    net_close(io->fd);
    io->fd = TC_INVALID_SOCKET;
  }
}
