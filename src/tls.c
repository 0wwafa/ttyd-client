#include "tls.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef TC_WITH_TLS

#ifndef _WIN32
#include <arpa/inet.h>
#endif
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "utils.h"

struct tc_tls {
  SSL *ssl;
  SSL_CTX *ctx;
};

static void tls_error_string(char *err, size_t err_len, const char *what) {
  unsigned long code = ERR_get_error();
  char detail[256] = "";
  if (code != 0) ERR_error_string_n(code, detail, sizeof(detail));
  if (detail[0] != '\0') {
    snprintf(err, err_len, "%s: %s", what, detail);
  } else {
    snprintf(err, err_len, "%s", what);
  }
}

static bool is_ip_address(const char *host) {
  unsigned char buf[16];
  return inet_pton(AF_INET, host, buf) == 1 || inet_pton(AF_INET6, host, buf) == 1;
}

static int wait_fd(socket_t sock, int timeout_ms, bool writing) {
  return writing ? net_wait_writable(sock, timeout_ms) : net_wait_readable(sock, timeout_ms);
}

bool tc_tls_available(void) { return true; }

tc_tls *tc_tls_start(socket_t sock, const char *host, const tls_options *options, char *err, size_t err_len) {
  if (!OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL)) {
    tls_error_string(err, err_len, "OpenSSL initialization failed");
    return NULL;
  }

  SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
  if (ctx == NULL) {
    tls_error_string(err, err_len, "cannot create TLS context");
    return NULL;
  }

  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
  SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY | SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

  if (options->verify) {
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    if (options->ca_file != NULL) {
      if (SSL_CTX_load_verify_locations(ctx, options->ca_file, NULL) != 1) {
        tls_error_string(err, err_len, "cannot load CA file");
        SSL_CTX_free(ctx);
        return NULL;
      }
    } else if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
      tls_error_string(err, err_len, "cannot load system CA certificates");
      SSL_CTX_free(ctx);
      return NULL;
    }
  } else {
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
  }

  if (options->cert_file != NULL) {
    if (SSL_CTX_use_certificate_file(ctx, options->cert_file, SSL_FILETYPE_PEM) != 1) {
      tls_error_string(err, err_len, "cannot load client certificate");
      SSL_CTX_free(ctx);
      return NULL;
    }
    const char *key = options->key_file != NULL ? options->key_file : options->cert_file;
    if (SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1) {
      tls_error_string(err, err_len, "cannot load client key");
      SSL_CTX_free(ctx);
      return NULL;
    }
  }

  SSL *ssl = SSL_new(ctx);
  if (ssl == NULL) {
    tls_error_string(err, err_len, "cannot create TLS session");
    SSL_CTX_free(ctx);
    return NULL;
  }

  const char *server_name = options->server_name != NULL ? options->server_name : host;
  if (server_name != NULL && !is_ip_address(server_name)) {
    SSL_set_tlsext_host_name(ssl, server_name);
  }
  if (options->verify && server_name != NULL) {
    X509_VERIFY_PARAM *param = SSL_get0_param(ssl);
    X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    if (is_ip_address(server_name)) {
      X509_VERIFY_PARAM_set1_ip_asc(param, server_name);
    } else {
      X509_VERIFY_PARAM_set1_host(param, server_name, 0);
    }
  }

  /* BIO_new_socket() keeps the SOCKET type intact on Windows (SOCKET is not an int) */
  BIO *bio = BIO_new_socket((int)(intptr_t)sock, BIO_NOCLOSE);
  if (bio == NULL) {
    tls_error_string(err, err_len, "cannot create TLS BIO");
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    return NULL;
  }
  SSL_set_bio(ssl, bio, bio);

  for (;;) {
    ERR_clear_error();
    int rc = SSL_connect(ssl);
    if (rc == 1) break;

    int sslerr = SSL_get_error(ssl, rc);
    if (sslerr == SSL_ERROR_WANT_READ || sslerr == SSL_ERROR_WANT_WRITE) {
      if (!wait_fd(sock, options->timeout_ms, sslerr == SSL_ERROR_WANT_WRITE)) {
        snprintf(err, err_len, "TLS handshake timed out");
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return NULL;
      }
      continue;
    }

    if (sslerr == SSL_ERROR_SSL && options->verify && SSL_get_verify_result(ssl) != X509_V_OK) {
      snprintf(err, err_len, "TLS certificate verification failed: %s",
               X509_verify_cert_error_string(SSL_get_verify_result(ssl)));
    } else {
      tls_error_string(err, err_len, "TLS handshake failed");
    }
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    return NULL;
  }

  tc_tls *tls = xmalloc(sizeof(tc_tls));
  tls->ssl = ssl;
  tls->ctx = ctx;
  return tls;
}

int tc_tls_read(tc_tls *tls, void *buf, size_t len) {
  if (len == 0) return 0;
  if (len > 0x7fffffff) len = 0x7fffffff;
  for (;;) {
    ERR_clear_error();
    int n = SSL_read(tls->ssl, buf, (int)len);
    if (n > 0) return n;
    int sslerr = SSL_get_error(tls->ssl, n);
    if (sslerr == SSL_ERROR_WANT_READ || sslerr == SSL_ERROR_WANT_WRITE) continue;
    if (sslerr == SSL_ERROR_ZERO_RETURN) return TC_IO_EOF;
    if (sslerr == SSL_ERROR_SYSCALL) {
      int code = net_last_error();
      if (code == 0) return TC_IO_EOF; /* clean EOF without close_notify */
      return TC_IO_ERROR;
    }
    return TC_IO_ERROR;
  }
}

int tc_tls_write(tc_tls *tls, const void *buf, size_t len) {
  if (len == 0) return 0;
  if (len > 0x7fffffff) len = 0x7fffffff;
  for (;;) {
    ERR_clear_error();
    int n = SSL_write(tls->ssl, buf, (int)len);
    if (n > 0) return n;
    int sslerr = SSL_get_error(tls->ssl, n);
    if (sslerr == SSL_ERROR_WANT_READ || sslerr == SSL_ERROR_WANT_WRITE) continue;
    return TC_IO_ERROR;
  }
}

int tc_tls_pending(tc_tls *tls) { return SSL_pending(tls->ssl); }

void tc_tls_free(tc_tls *tls) {
  if (tls == NULL) return;
  if (tls->ssl != NULL) {
    SSL_shutdown(tls->ssl);
    SSL_free(tls->ssl);
  }
  if (tls->ctx != NULL) SSL_CTX_free(tls->ctx);
  free(tls);
}

#else /* !TC_WITH_TLS */

struct tc_tls {
  int unused;
};

bool tc_tls_available(void) { return false; }

tc_tls *tc_tls_start(socket_t sock, const char *host, const tls_options *options, char *err, size_t err_len) {
  (void)sock;
  (void)host;
  (void)options;
  snprintf(err, err_len, "this build of ttyd-client does not support TLS (rebuild with -DTC_WITH_TLS=ON)");
  return NULL;
}

int tc_tls_read(tc_tls *tls, void *buf, size_t len) {
  (void)tls;
  (void)buf;
  (void)len;
  return TC_IO_ERROR;
}

int tc_tls_write(tc_tls *tls, const void *buf, size_t len) {
  (void)tls;
  (void)buf;
  (void)len;
  return TC_IO_ERROR;
}

int tc_tls_pending(tc_tls *tls) {
  (void)tls;
  return 0;
}

void tc_tls_free(tc_tls *tls) { (void)tls; }

#endif
