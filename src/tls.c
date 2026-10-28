#include "tls.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <unistd.h>
#endif
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#endif

#include "log.h"
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

#ifndef _WIN32
/* Count the certificates that are actually present in the trust store.
 * SSL_CTX_set_default_verify_paths() returns success even when nothing was
 * loaded, for example when the compiled in OPENSSLDIR does not exist. */
static size_t store_cert_count(SSL_CTX *ctx) {
  STACK_OF(X509_OBJECT) *objects = X509_STORE_get0_objects(SSL_CTX_get_cert_store(ctx));
  if (objects == NULL) return 0;

  size_t count = 0;
  for (int i = 0; i < sk_X509_OBJECT_num(objects); i++) {
    if (X509_OBJECT_get_type(sk_X509_OBJECT_value(objects, i)) == X509_LU_X509) count++;
  }
  return count;
}

/* Load the system trust store. Static OpenSSL builds have OPENSSLDIR compiled
 * in as the directory they were built in, which usually does not exist on the
 * machine that runs the binary, so fall back to the conventional locations of
 * the system CA bundle. */
static bool load_system_ca(SSL_CTX *ctx) {
  const char *env_file = getenv("SSL_CERT_FILE");
  const char *env_dir = getenv("SSL_CERT_DIR");
  /* Explicit overrides are honoured, even when they end up loading nothing. */
  bool env_ca = (env_file != NULL && env_file[0] != '\0') || (env_dir != NULL && env_dir[0] != '\0');

  if (SSL_CTX_set_default_verify_paths(ctx) == 1) {
    size_t count = store_cert_count(ctx);
    if (count > 0) {
      log_debug("loaded %zu CA certificate(s) from the OpenSSL default paths\n", count);
      return true;
    }
    if (env_ca) {
      log_debug("using CA certificates from SSL_CERT_FILE/SSL_CERT_DIR\n");
      return true;
    }
  }

  if (!env_ca) {
    static const char *const bundles[] = {
        "/etc/ssl/certs/ca-certificates.crt",                /* Debian, Ubuntu, Arch, Alpine, Gentoo */
        "/etc/pki/tls/certs/ca-bundle.crt",                  /* Fedora, RHEL, CentOS */
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", /* Fedora, RHEL */
        "/etc/ssl/ca-bundle.pem",                            /* openSUSE */
        "/etc/ssl/cert.pem",                                 /* Alpine, macOS, FreeBSD, OpenBSD */
        "/usr/local/etc/ssl/cert.pem",                       /* FreeBSD ports */
        "/usr/local/share/certs/ca-root-nss.crt",            /* FreeBSD ca_root_nss */
        "/data/data/com.termux/files/usr/etc/tls/cert.pem",  /* Termux */
    };
    for (size_t i = 0; i < sizeof(bundles) / sizeof(bundles[0]); i++) {
      ERR_clear_error();
      if (SSL_CTX_load_verify_locations(ctx, bundles[i], NULL) == 1) {
        size_t count = store_cert_count(ctx);
        if (count > 0) {
          log_debug("loaded %zu CA certificate(s) from %s\n", count, bundles[i]);
          return true;
        }
      }
    }

    static const char *const dirs[] = {
        "/etc/ssl/certs",
        "/etc/pki/tls/certs",
        "/usr/local/etc/ssl/certs",
        "/system/etc/security/cacerts",                  /* Android */
        "/data/data/com.termux/files/usr/etc/tls/certs", /* Termux */
    };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
      if (access(dirs[i], R_OK | X_OK) != 0) continue;
      ERR_clear_error();
      if (SSL_CTX_load_verify_locations(ctx, NULL, dirs[i]) == 1) {
        log_debug("using CA certificate directory %s\n", dirs[i]);
        return true;
      }
    }
  }

  ERR_clear_error();
  return false;
}
#endif

#ifdef _WIN32
/* Add the certificates from one Windows ROOT store to the OpenSSL trust store.
 * The Windows ROOT store is the trust-anchor store; the CA store contains
 * intermediates and must not be treated as trusted roots. */
static int load_windows_root_store(SSL_CTX *ctx, DWORD location) {
  HCERTSTORE store =
      CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, location | CERT_STORE_READONLY_FLAG, L"ROOT");
  if (store == NULL) return 0;

  int loaded = 0;
  PCCERT_CONTEXT cert_ctx = NULL;
  while ((cert_ctx = CertEnumCertificatesInStore(store, cert_ctx)) != NULL) {
    const unsigned char *der = cert_ctx->pbCertEncoded;
    X509 *cert = d2i_X509(NULL, &der, (long)cert_ctx->cbCertEncoded);
    if (cert == NULL) {
      ERR_clear_error();
      continue;
    }
    if (X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx), cert) == 1) {
      loaded++;
    } else {
      /* Duplicates are expected when the same root is in both locations. */
      ERR_clear_error();
    }
    X509_free(cert);
  }

  CertCloseStore(store, 0);
  return loaded;
}

static int load_windows_root_certs(SSL_CTX *ctx) {
  int loaded = load_windows_root_store(ctx, CERT_SYSTEM_STORE_CURRENT_USER);
  loaded += load_windows_root_store(ctx, CERT_SYSTEM_STORE_LOCAL_MACHINE);
  log_debug("loaded %d CA certificate(s) from the Windows certificate store\n", loaded);
  return loaded;
}
#endif

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
    } else {
#ifdef _WIN32
      bool have_ca = SSL_CTX_set_default_verify_paths(ctx) == 1;
      if (!have_ca) ERR_clear_error();
      if (load_windows_root_certs(ctx) > 0) have_ca = true;
      if (!have_ca) {
        snprintf(err, err_len,
                 "cannot load system CA certificates (Windows certificate store unavailable, use -A/--ssl-ca)");
        SSL_CTX_free(ctx);
        return NULL;
      }
#else
      if (!load_system_ca(ctx)) {
        snprintf(err, err_len, "cannot load system CA certificates (no CA bundle found, use -A/--ssl-ca)");
        SSL_CTX_free(ctx);
        return NULL;
      }
#endif
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
