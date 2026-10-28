#ifndef TTYD_CLIENT_HTTP_H
#define TTYD_CLIENT_HTTP_H

#include <stdbool.h>
#include <stddef.h>

#include "buffer.h"
#include "header.h"
#include "io.h"

/* A very small HTTP/1.1 client, used for the ttyd `/token` endpoint
 * (and the proxy CONNECT tunnel, implemented in io.c). */

typedef tc_header http_header;

typedef struct {
  int status;
  char reason[128];
  http_header *headers;
  size_t header_count;
  buffer_t body;
} http_response;

/* Perform a request on an already established connection.
 * `target` is the request target, eg: /token or /my/base/token.
 * Returns false and fills `err` on transport failure. */
bool http_request(tc_io *io, const char *method, const char *target, const char *host, int port,
                  const http_header *headers, size_t header_count, http_response *response, char *err, size_t err_len);

/* Free the memory held by a response */
void http_response_free(http_response *response);

/* Case insensitive header lookup, returns NULL when not present */
const char *http_response_header(const http_response *response, const char *name);

#endif  // TTYD_CLIENT_HTTP_H
