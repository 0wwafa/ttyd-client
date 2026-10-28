#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat.h"
#include "log.h"
#include "utils.h"
#include "version.h"

#define HTTP_MAX_HEAD (64u * 1024u)
#define HTTP_MAX_BODY (8u * 1024u * 1024u)

static void add_header(http_response *response, const char *name, size_t name_len, const char *value,
                       size_t value_len) {
  response->headers = xrealloc(response->headers, (response->header_count + 1) * sizeof(http_header));
  http_header *header = &response->headers[response->header_count++];
  header->name = xstrndup(name, name_len);
  header->value = xstrndup(value, value_len);
}

const char *http_response_header(const http_response *response, const char *name) {
  if (response == NULL || name == NULL) return NULL;
  for (size_t i = 0; i < response->header_count; i++) {
    if (str_eq_ignore_case(response->headers[i].name, name)) return response->headers[i].value;
  }
  return NULL;
}

void http_response_free(http_response *response) {
  if (response == NULL) return;
  for (size_t i = 0; i < response->header_count; i++) {
    free(response->headers[i].name);
    free(response->headers[i].value);
  }
  free(response->headers);
  buffer_free(&response->body);
  memset(response, 0, sizeof(*response));
}

/* Read a single line (terminated by \n) from the stream into `line`.
 * Returns false on error/EOF. */
static bool read_line(tc_io *io, buffer_t *line, char *err, size_t err_len) {
  buffer_reset(line);
  char c;
  for (;;) {
    int rc = io_read(io, &c, 1);
    if (rc == TC_IO_EOF) {
      snprintf(err, err_len, "connection closed while reading the HTTP response");
      return false;
    }
    if (rc == TC_IO_ERROR) {
      snprintf(err, err_len, "network error while reading the HTTP response");
      return false;
    }
    if (c == '\n') break;
    if (line->len < HTTP_MAX_HEAD) buffer_append_char(line, c);
  }
  while (line->len > 0 && (line->data[line->len - 1] == '\r')) line->len--;
  if (line->data != NULL) line->data[line->len] = '\0';
  return true;
}

bool http_request(tc_io *io, const char *method, const char *target, const char *host, int port,
                  const http_header *headers, size_t header_count, http_response *response, char *err, size_t err_len) {
  memset(response, 0, sizeof(*response));
  buffer_init(&response->body);

  char host_header[300];
  bool default_port = (port == 80 && io->tls == NULL) || (port == 443 && io->tls != NULL);
  if (default_port) {
    snprintf(host_header, sizeof(host_header), "%s", host);
  } else if (strchr(host, ':') != NULL) {
    snprintf(host_header, sizeof(host_header), "[%s]:%d", host, port);
  } else {
    snprintf(host_header, sizeof(host_header), "%s:%d", host, port);
  }

  buffer_t request;
  buffer_init(&request);
  buffer_appendf(&request, "%s %s HTTP/1.1\r\n", method, target);
  buffer_appendf(&request, "Host: %s\r\n", host_header);
  buffer_appendf(&request, "User-Agent: ttyd-client/%s\r\n", TTYD_CLIENT_VERSION);
  buffer_append_str(&request, "Accept: application/json, */*\r\n");
  buffer_append_str(&request, "Connection: close\r\n");
  for (size_t i = 0; i < header_count; i++) {
    if (headers[i].name != NULL && headers[i].value != NULL) {
      buffer_appendf(&request, "%s: %s\r\n", headers[i].name, headers[i].value);
    }
  }
  buffer_append_str(&request, "\r\n");

  log_debug("http request:\n%.*s", (int)request.len, request.data);

  if (!io_write_all(io, request.data, request.len)) {
    snprintf(err, err_len, "cannot send HTTP request");
    buffer_free(&request);
    return false;
  }
  buffer_free(&request);

  buffer_t line;
  buffer_init(&line);

  /* Status line */
  if (!read_line(io, &line, err, err_len)) {
    buffer_free(&line);
    return false;
  }
  if (sscanf(line.data, "HTTP/%*d.%*d %d %127[^\r\n]", &response->status, response->reason) < 1) {
    snprintf(err, err_len, "invalid HTTP status line: %s", line.data);
    buffer_free(&line);
    return false;
  }

  /* Headers */
  for (;;) {
    if (!read_line(io, &line, err, err_len)) {
      buffer_free(&line);
      http_response_free(response);
      return false;
    }
    if (line.len == 0) break;
    char *colon = strchr(line.data, ':');
    if (colon == NULL) continue;
    const char *name = line.data;
    size_t name_len = (size_t)(colon - line.data);
    const char *value = colon + 1;
    while (*value == ' ' || *value == '\t') value++;
    size_t value_len = strlen(value);
    while (value_len > 0 && (value[value_len - 1] == ' ' || value[value_len - 1] == '\t')) value_len--;
    add_header(response, name, name_len, value, value_len);
  }

  /* Body */
  const char *transfer = http_response_header(response, "Transfer-Encoding");
  const char *length = http_response_header(response, "Content-Length");

  if (transfer != NULL && strstr(transfer, "chunked") != NULL) {
    for (;;) {
      if (!read_line(io, &line, err, err_len)) {
        buffer_free(&line);
        http_response_free(response);
        return false;
      }
      long chunk = strtol(line.data, NULL, 16);
      if (chunk <= 0) {
        /* consume the trailer */
        for (;;) {
          if (!read_line(io, &line, err, err_len)) break;
          if (line.len == 0) break;
        }
        break;
      }
      if (response->body.len + (size_t)chunk > HTTP_MAX_BODY) {
        snprintf(err, err_len, "HTTP response body too large");
        buffer_free(&line);
        http_response_free(response);
        return false;
      }
      size_t want = (size_t)chunk;
      while (want > 0) {
        char buf[4096];
        size_t n = want < sizeof(buf) ? want : sizeof(buf);
        int rc = io_read(io, buf, n);
        if (rc <= 0) {
          snprintf(err, err_len, "connection closed while reading a chunk");
          buffer_free(&line);
          http_response_free(response);
          return false;
        }
        buffer_append(&response->body, buf, (size_t)rc);
        want -= (size_t)rc;
      }
      read_line(io, &line, err, err_len); /* trailing CRLF */
    }
  } else if (length != NULL) {
    long want = strtol(length, NULL, 10);
    if (want < 0 || (size_t)want > HTTP_MAX_BODY) {
      snprintf(err, err_len, "invalid Content-Length: %s", length);
      buffer_free(&line);
      http_response_free(response);
      return false;
    }
    while ((long)response->body.len < want) {
      char buf[4096];
      size_t n = (size_t)want - response->body.len;
      if (n > sizeof(buf)) n = sizeof(buf);
      int rc = io_read(io, buf, n);
      if (rc <= 0) break;
      buffer_append(&response->body, buf, (size_t)rc);
    }
  } else {
    char buf[4096];
    for (;;) {
      int rc = io_read(io, buf, sizeof(buf));
      if (rc <= 0) break;
      if (response->body.len + (size_t)rc > HTTP_MAX_BODY) break;
      buffer_append(&response->body, buf, (size_t)rc);
    }
  }

  buffer_free(&line);
  return true;
}
