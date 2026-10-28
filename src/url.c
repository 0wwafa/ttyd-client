#include "url.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils.h"

/* Default port used by ttyd when nothing else is specified */
#define TTYD_DEFAULT_PORT 7681

static int scheme_default_port(const char *scheme, bool had_scheme) {
  /* Without an explicit scheme a bare "host[:port]" means a local ttyd,
   * which listens on 7681 by default. */
  if (!had_scheme) return TTYD_DEFAULT_PORT;
  if (str_eq_ignore_case(scheme, "ws") || str_eq_ignore_case(scheme, "http")) return 80;
  if (str_eq_ignore_case(scheme, "wss") || str_eq_ignore_case(scheme, "https")) return 443;
  return TTYD_DEFAULT_PORT;
}

static void set_error(char *err, size_t err_len, const char *msg) {
  if (err != NULL && err_len > 0) snprintf(err, err_len, "%s", msg);
}

bool url_parse(const char *input, bool default_tls, tc_url *url, char *err, size_t err_len) {
  if (input == NULL || *input == '\0') {
    set_error(err, err_len, "empty url");
    return false;
  }
  memset(url, 0, sizeof(*url));

  const char *rest = input;
  const char *scheme_sep = strstr(input, "://");
  bool had_scheme = false;

  if (scheme_sep != NULL) {
    had_scheme = true;
    url->scheme = xstrndup(input, (size_t)(scheme_sep - input));
    lowercase(url->scheme);
    rest = scheme_sep + 3;
  } else {
    url->scheme = xstrdup(default_tls ? "wss" : "ws");
  }

  if (!strcmp(url->scheme, "wss") || !strcmp(url->scheme, "https")) {
    url->tls = true;
  } else if (!strcmp(url->scheme, "ws") || !strcmp(url->scheme, "http")) {
    url->tls = false;
  } else {
    char msg[128];
    snprintf(msg, sizeof(msg), "unsupported url scheme: %s (expected ws, wss, http or https)", url->scheme);
    set_error(err, err_len, msg);
    url_free(url);
    return false;
  }

  /* Split off the path/query, taking care of IPv6 literals in brackets */
  const char *host_start = rest;
  const char *path_start = NULL;
  const char *scan = rest;
  if (*scan == '[') {
    const char *close = strchr(scan, ']');
    if (close == NULL) {
      set_error(err, err_len, "unterminated IPv6 address");
      url_free(url);
      return false;
    }
    scan = close + 1;
  }
  for (; *scan; scan++) {
    if (*scan == '/' || *scan == '?' || *scan == '#') {
      path_start = scan;
      break;
    }
  }
  const char *authority_end = path_start != NULL ? path_start : scan;

  /* userinfo */
  const char *at = NULL;
  for (const char *p = host_start; p < authority_end; p++) {
    if (*p == '@') at = p;
  }
  const char *host_begin = host_start;
  if (at != NULL) {
    const char *colon = memchr(host_start, ':', (size_t)(at - host_start));
    if (colon != NULL) {
      url->user = xstrndup(host_start, (size_t)(colon - host_start));
      url->password = xstrndup(colon + 1, (size_t)(at - colon - 1));
    } else {
      url->user = xstrndup(host_start, (size_t)(at - host_start));
    }
    host_begin = at + 1;
  }

  /* host and optional port */
  const char *host_end = authority_end;
  const char *port_start = NULL;
  if (host_begin < authority_end && *host_begin == '[') {
    const char *close = memchr(host_begin, ']', (size_t)(authority_end - host_begin));
    if (close == NULL) {
      set_error(err, err_len, "unterminated IPv6 address");
      url_free(url);
      return false;
    }
    url->host = xstrndup(host_begin + 1, (size_t)(close - host_begin - 1));
    if (close + 1 < authority_end && close[1] == ':') port_start = close + 2;
  } else {
    const char *colon = NULL;
    for (const char *p = host_begin; p < authority_end; p++) {
      if (*p == ':') colon = p;
    }
    if (colon != NULL) {
      host_end = colon;
      port_start = colon + 1;
    }
    url->host = xstrndup(host_begin, (size_t)(host_end - host_begin));
  }

  if (url->host == NULL || url->host[0] == '\0') {
    set_error(err, err_len, "missing host in url");
    url_free(url);
    return false;
  }

  if (port_start != NULL && port_start < authority_end) {
    long port = 0;
    char *tmp = xstrndup(port_start, (size_t)(authority_end - port_start));
    bool ok = parse_long(tmp, &port) && port > 0 && port < 65536;
    free(tmp);
    if (!ok) {
      set_error(err, err_len, "invalid port in url");
      url_free(url);
      return false;
    }
    url->port = (int)port;
  } else {
    url->port = scheme_default_port(url->scheme, had_scheme);
  }

  /* path and query */
  if (path_start != NULL) {
    const char *string_end = input + strlen(input);
    const char *q = strchr(path_start, '?');
    const char *hash = strchr(path_start, '#');
    const char *path_end = string_end;
    if (q != NULL && q < path_end) path_end = q;
    if (hash != NULL && hash < path_end) path_end = hash;

    if (path_end > path_start) {
      url->path = xstrndup(path_start, (size_t)(path_end - path_start));
    }
    if (q != NULL && (hash == NULL || q < hash)) {
      const char *query_end = hash != NULL ? hash : string_end;
      url->query = xstrndup(q + 1, (size_t)(query_end - q - 1));
    }
  }
  if (url->path == NULL || url->path[0] == '\0') {
    free(url->path);
    url->path = xstrdup("/");
  }

  return true;
}

void url_free(tc_url *url) {
  if (url == NULL) return;
  free(url->scheme);
  free(url->host);
  free(url->path);
  free(url->query);
  free(url->user);
  free(url->password);
  memset(url, 0, sizeof(*url));
}

void url_endpoints(const tc_url *url, tc_endpoints *endpoints) {
  memset(endpoints, 0, sizeof(*endpoints));

  size_t len = strlen(url->path);
  char *base = xstrdup(url->path);

  /* Trim trailing slashes, but keep the root path as "" */
  while (len > 0 && base[len - 1] == '/') base[--len] = '\0';

  /* A url may point at the websocket endpoint (/ws) or the token endpoint
   * (/token) directly; the base path is what remains in front of them. */
  if (endswith(base, "/ws")) {
    base[len - 3] = '\0';
  } else if (endswith(base, "/token")) {
    base[len - 6] = '\0';
  }
  len = strlen(base);

  endpoints->base = xstrdup(base);
  size_t ws_len = len + 4;
  endpoints->ws = xmalloc(ws_len);
  snprintf(endpoints->ws, ws_len, "%s/ws", base);
  size_t token_len = len + 7;
  endpoints->token = xmalloc(token_len);
  snprintf(endpoints->token, token_len, "%s/token", base);

  free(base);
}

void url_endpoints_free(tc_endpoints *endpoints) {
  if (endpoints == NULL) return;
  free(endpoints->base);
  free(endpoints->ws);
  free(endpoints->token);
  memset(endpoints, 0, sizeof(*endpoints));
}

char *url_encode(const char *str) {
  if (str == NULL) return xstrdup("");
  size_t len = strlen(str);
  char *out = xmalloc(len * 3 + 1);
  size_t o = 0;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)str[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out[o++] = (char)c;
    } else {
      static const char hex[] = "0123456789ABCDEF";
      out[o++] = '%';
      out[o++] = hex[c >> 4];
      out[o++] = hex[c & 0x0f];
    }
  }
  out[o] = '\0';
  return out;
}

void url_append_query(char **query, const char *key, const char *value) {
  char *encoded = url_encode(value != NULL ? value : "");
  char *key_encoded = url_encode(key != NULL ? key : "");
  size_t need = strlen(*query ? *query : "") + strlen(key_encoded) + strlen(encoded) + 4;
  char *result = xmalloc(need);
  snprintf(result, need, "%s%s%s=%s", *query ? *query : "", (*query && **query) ? "&" : "", key_encoded, encoded);
  free(*query);
  *query = result;
  free(encoded);
  free(key_encoded);
}
