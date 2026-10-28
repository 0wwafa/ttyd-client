#ifndef TTYD_CLIENT_URL_H
#define TTYD_CLIENT_URL_H

#include <stdbool.h>
#include <stddef.h>

/* Parsed ttyd url, eg: ws://user:pass@localhost:7681/my/base/path?arg=foo */
typedef struct {
  char *scheme;   /* ws, wss, http or https */
  char *host;     /* host name or address, without [] for IPv6 literals */
  int port;       /* resolved port */
  char *path;     /* request path, always starts with '/', never NULL */
  char *query;    /* query string without '?', NULL when absent */
  char *user;     /* userinfo user, NULL when absent */
  char *password; /* userinfo password, NULL when absent */
  bool tls;       /* true for wss and https */
} tc_url;

/* ttyd endpoints derived from the url path, see the --base-path server option */
typedef struct {
  char *base;  /* base path, "" or starts with '/', no trailing slash */
  char *ws;    /* websocket endpoint */
  char *token; /* token endpoint */
} tc_endpoints;

/* Parse `input` into `url`. `default_tls` is used when the input has no scheme.
 * On failure returns false and fills `err` (when non NULL). */
bool url_parse(const char *input, bool default_tls, tc_url *url, char *err, size_t err_len);

/* Free the members of a parsed url */
void url_free(tc_url *url);

/* Derive the ttyd endpoints from the url path */
void url_endpoints(const tc_url *url, tc_endpoints *endpoints);

/* Free the members of an endpoints struct */
void url_endpoints_free(tc_endpoints *endpoints);

/* Percent encode a string for use in a query string value, returns a new string */
char *url_encode(const char *str);

/* Append `key=value` (percent encoded) to a query string, handling '&' and '?'
 * separators. The query buffer may start as NULL. */
void url_append_query(char **query, const char *key, const char *value);

#endif  // TTYD_CLIENT_URL_H
