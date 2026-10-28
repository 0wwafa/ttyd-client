#include "ws.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "b64.h"
#include "compat.h"
#include "log.h"
#include "sha1.h"
#include "utils.h"
#include "version.h"

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_DEFAULT_MAX_MESSAGE (64u * 1024u * 1024u)
#define WS_MAX_HANDSHAKE (64u * 1024u)

void ws_init(ws_conn *ws, tc_io *io) {
  memset(ws, 0, sizeof(*ws));
  ws->io = io;
  buffer_init(&ws->in);
  buffer_init(&ws->fragment);
  ws->close_code = WS_CLOSE_NO_STATUS;
  ws->max_message = WS_DEFAULT_MAX_MESSAGE;
}

void ws_free(ws_conn *ws) {
  if (ws == NULL) return;
  buffer_free(&ws->in);
  buffer_free(&ws->fragment);
}

void ws_message_free(ws_message *message) {
  if (message == NULL) return;
  buffer_free(&message->payload);
  message->opcode = 0;
}

/* ------------------------------------------------------------------------- */
/* handshake                                                                  */
/* ------------------------------------------------------------------------- */

/* Find a header value in a raw HTTP header block (case insensitive).
 * The header block is not modified. */
static char *find_header(const char *head, const char *name, char *out, size_t out_len) {
  size_t name_len = strlen(name);
  const char *line = head;

  while (*line) {
    const char *eol = strchr(line, '\n');
    size_t line_len = eol != NULL ? (size_t)(eol - line) : strlen(line);
    if (line_len > 0 && line[line_len - 1] == '\r') line_len--;

    if (line_len > name_len && strncasecmp(line, name, name_len) == 0 && line[name_len] == ':') {
      const char *value = line + name_len + 1;
      size_t value_len = line_len - (size_t)(value - line);
      while (value_len > 0 && (*value == ' ' || *value == '\t')) {
        value++;
        value_len--;
      }
      while (value_len > 0 && (value[value_len - 1] == ' ' || value[value_len - 1] == '\t')) value_len--;
      size_t copy = value_len < out_len - 1 ? value_len : out_len - 1;
      memcpy(out, value, copy);
      out[copy] = '\0';
      return out;
    }

    if (eol == NULL) break;
    line = eol + 1;
    if (*line == '\r' && line[1] == '\n') break;
    if (*line == '\n') break;
  }
  return NULL;
}

bool ws_handshake(ws_conn *ws, const char *host, int port, const char *target, const ws_header *headers,
                  size_t header_count, char *err, size_t err_len) {
  unsigned char key_bytes[16];
  if (!random_bytes(key_bytes, sizeof(key_bytes))) {
    snprintf(err, err_len, "cannot generate websocket key");
    return false;
  }
  char key[32];
  base64_encode(key_bytes, sizeof(key_bytes), key, sizeof(key));

  char host_header[300];
  bool default_port = (port == 80 || port == 443);
  if (default_port) {
    snprintf(host_header, sizeof(host_header), "%s", host);
  } else if (strchr(host, ':') != NULL) {
    snprintf(host_header, sizeof(host_header), "[%s]:%d", host, port);
  } else {
    snprintf(host_header, sizeof(host_header), "%s:%d", host, port);
  }

  buffer_t request;
  buffer_init(&request);
  buffer_appendf(&request, "GET %s HTTP/1.1\r\n", target);
  buffer_appendf(&request, "Host: %s\r\n", host_header);
  buffer_append_str(&request, "Upgrade: websocket\r\n");
  buffer_append_str(&request, "Connection: Upgrade\r\n");
  buffer_appendf(&request, "Sec-WebSocket-Key: %s\r\n", key);
  buffer_append_str(&request, "Sec-WebSocket-Version: 13\r\n");
  buffer_append_str(&request, "Sec-WebSocket-Protocol: tty\r\n");
  buffer_appendf(&request, "User-Agent: ttyd-client/%s\r\n", TTYD_CLIENT_VERSION);
  if (ws->io->tls != NULL) {
    buffer_appendf(&request, "Origin: https://%s\r\n", host_header);
  } else {
    buffer_appendf(&request, "Origin: http://%s\r\n", host_header);
  }
  for (size_t i = 0; i < header_count; i++) {
    if (headers[i].name != NULL && headers[i].value != NULL) {
      buffer_appendf(&request, "%s: %s\r\n", headers[i].name, headers[i].value);
    }
  }
  buffer_append_str(&request, "\r\n");

  log_debug("websocket handshake request:\n%.*s", (int)request.len, request.data);

  if (!io_write_all(ws->io, request.data, request.len)) {
    snprintf(err, err_len, "cannot send websocket handshake");
    buffer_free(&request);
    return false;
  }
  buffer_free(&request);

  /* Read the response head */
  char head[WS_MAX_HANDSHAKE];
  size_t len = 0;
  for (;;) {
    if (len + 1 >= sizeof(head)) {
      snprintf(err, err_len, "websocket handshake response too large");
      return false;
    }
    if (!io_wait_readable(ws->io, -1)) {
      snprintf(err, err_len, "websocket handshake timed out");
      return false;
    }
    int rc = io_read(ws->io, head + len, sizeof(head) - len - 1);
    if (rc == TC_IO_ERROR) {
      snprintf(err, err_len, "connection closed during websocket handshake");
      return false;
    }
    if (rc == TC_IO_EOF) {
      snprintf(err, err_len, "connection closed during websocket handshake");
      return false;
    }
    len += (size_t)rc;
    head[len] = '\0';
    char *end = strstr(head, "\r\n\r\n");
    if (end != NULL) {
      size_t head_len = (size_t)(end + 4 - head);
      /* Anything after the header block already belongs to the websocket stream */
      buffer_append(&ws->in, head + head_len, len - head_len);
      head[head_len] = '\0';
      break;
    }
  }

  int status = 0;
  char reason[128] = "";
  if (sscanf(head, "HTTP/%*d.%*d %d %127[^\r\n]", &status, reason) < 1) {
    snprintf(err, err_len, "invalid HTTP response from server");
    return false;
  }

  if (status != 101) {
    char msg[512] = "";
    char *body = strstr(head, "\r\n\r\n");
    if (body != NULL) {
      str_trim(body + 4);
      snprintf(msg, sizeof(msg), ": %s", body + 4);
      if (strlen(msg) > 200) msg[200] = '\0';
    }
    if (status == 401) {
      snprintf(err, err_len, "websocket upgrade rejected: HTTP 401 Unauthorized (check --credential)");
    } else if (status == 403) {
      snprintf(err, err_len, "websocket upgrade rejected: HTTP 403 Forbidden%s", msg);
    } else if (status == 404) {
      snprintf(err, err_len, "websocket upgrade rejected: HTTP 404 Not Found (check the url path)");
    } else {
      snprintf(err, err_len, "websocket upgrade rejected: HTTP %d %s%s", status, reason, msg);
    }
    return false;
  }

  char value[512];
  if (find_header(head, "Upgrade", value, sizeof(value)) == NULL || !str_eq_ignore_case(value, "websocket")) {
    snprintf(err, err_len, "server did not upgrade to websocket");
    return false;
  }

  /* Verify Sec-WebSocket-Accept = base64(sha1(key + GUID)) */
  char expected_src[128];
  snprintf(expected_src, sizeof(expected_src), "%s" WS_GUID, key);
  unsigned char digest[SHA1_DIGEST_SIZE];
  sha1(expected_src, strlen(expected_src), digest);
  char expected[64];
  base64_encode(digest, sizeof(digest), expected, sizeof(expected));

  if (find_header(head, "Sec-WebSocket-Accept", value, sizeof(value)) == NULL || strcmp(value, expected) != 0) {
    snprintf(err, err_len, "invalid Sec-WebSocket-Accept from server");
    return false;
  }

  if (find_header(head, "Sec-WebSocket-Protocol", value, sizeof(value)) != NULL && !str_eq_ignore_case(value, "tty")) {
    snprintf(err, err_len, "server selected unexpected subprotocol: %s", value);
    return false;
  }

  return true;
}

/* ------------------------------------------------------------------------- */
/* frame reading                                                              */
/* ------------------------------------------------------------------------- */

typedef struct {
  int opcode;
  bool fin;
  const unsigned char *payload;
  size_t payload_len;
  size_t frame_len; /* total bytes consumed from the input buffer */
} ws_frame;

/* Try to parse one complete frame out of `buf`.
 * Returns false when more data is needed; sets `error` on protocol violations. */
static bool parse_frame(const unsigned char *buf, size_t len, ws_frame *frame, const char **error) {
  if (len < 2) return false;

  bool fin = (buf[0] & 0x80) != 0;
  int rsv = (buf[0] & 0x70) >> 4;
  int opcode = buf[0] & 0x0f;
  bool masked = (buf[1] & 0x80) != 0;
  uint64_t payload_len = buf[1] & 0x7f;
  size_t offset = 2;

  if (rsv != 0) {
    *error = "server sent RSV bits without a negotiated extension";
    return false;
  }
  if (masked) {
    *error = "server sent a masked frame";
    return false;
  }

  if (payload_len == 126) {
    if (len < offset + 2) return false;
    payload_len = ((uint64_t)buf[offset] << 8) | buf[offset + 1];
    offset += 2;
  } else if (payload_len == 127) {
    if (len < offset + 8) return false;
    payload_len = 0;
    for (int i = 0; i < 8; i++) payload_len = (payload_len << 8) | buf[offset + i];
    offset += 8;
  }

  if (payload_len > (uint64_t)(SIZE_MAX / 2)) {
    *error = "frame too large";
    return false;
  }
  if (len < offset + (size_t)payload_len) return false;

  frame->fin = fin;
  frame->opcode = opcode;
  frame->payload = buf + offset;
  frame->payload_len = (size_t)payload_len;
  frame->frame_len = offset + (size_t)payload_len;
  return true;
}

static int read_more(ws_conn *ws, int timeout_ms) {
  char tmp[16384];
  if (timeout_ms == 0) {
    /* Never block: only read when the socket already has data waiting */
    if (!io_wait_readable(ws->io, 0)) return TC_IO_AGAIN;
    int rc = io_read(ws->io, tmp, sizeof(tmp));
    if (rc > 0) buffer_append(&ws->in, tmp, (size_t)rc);
    return rc;
  }
  if (!io_wait_readable(ws->io, timeout_ms)) return TC_IO_AGAIN;
  int rc = io_read(ws->io, tmp, sizeof(tmp));
  if (rc > 0) buffer_append(&ws->in, tmp, (size_t)rc);
  return rc;
}

static ws_result handle_control(ws_conn *ws, const ws_frame *frame, ws_message *message, char *err, size_t err_len) {
  switch (frame->opcode) {
    case WS_OPCODE_PING:
      log_debug("received ping (%zu bytes)\n", frame->payload_len);
      ws_send_pong(ws, frame->payload, frame->payload_len);
      return WS_MSG_PING;
    case WS_OPCODE_PONG:
      log_debug("received pong (%zu bytes)\n", frame->payload_len);
      return WS_MSG_PONG;
    case WS_OPCODE_CLOSE: {
      ws->closing = true;
      if (frame->payload_len >= 2) {
        ws->close_code = ((int)frame->payload[0] << 8) | frame->payload[1];
        size_t reason_len = frame->payload_len - 2;
        if (reason_len > 123) reason_len = 123;
        char reason[124];
        memcpy(reason, frame->payload + 2, reason_len);
        reason[reason_len] = '\0';
        log_debug("received close frame: code %d, reason: %s\n", ws->close_code, reason);
      } else {
        ws->close_code = WS_CLOSE_NO_STATUS;
      }
      return WS_MSG_CLOSE;
    }
    default:
      snprintf(err, err_len, "unexpected control opcode 0x%x", frame->opcode);
      (void)message;
      return WS_MSG_ERROR;
  }
}

ws_result ws_read_message(ws_conn *ws, ws_message *message, int timeout_ms, char *err, size_t err_len) {
  memset(message, 0, sizeof(*message));
  buffer_init(&message->payload);

  uint64_t deadline = timeout_ms > 0 ? now_ms() + (uint64_t)timeout_ms : 0;

  for (;;) {
    ws_frame frame;
    const char *parse_error = NULL;

    if (ws->in.len > 0 && parse_frame((const unsigned char *)ws->in.data, ws->in.len, &frame, &parse_error)) {
      switch (frame.opcode) {
        case WS_OPCODE_CONTINUATION:
          if (ws->fragment_opcode == 0) {
            snprintf(err, err_len, "unexpected continuation frame");
            return WS_MSG_ERROR;
          }
          if (ws->fragment.len + frame.payload_len > ws->max_message) {
            snprintf(err, err_len, "message exceeds the %zu byte limit", ws->max_message);
            return WS_MSG_ERROR;
          }
          buffer_append(&ws->fragment, frame.payload, frame.payload_len);
          buffer_consume(&ws->in, frame.frame_len);
          if (!frame.fin) break;
          message->opcode = ws->fragment_opcode;
          message->payload = ws->fragment;
          buffer_init(&ws->fragment);
          ws->fragment_opcode = 0;
          return WS_MSG_DATA;

        case WS_OPCODE_TEXT:
        case WS_OPCODE_BINARY:
          if (ws->fragment_opcode != 0) {
            snprintf(err, err_len, "new data frame while a fragmented message is in progress");
            return WS_MSG_ERROR;
          }
          if (frame.payload_len > ws->max_message) {
            snprintf(err, err_len, "message exceeds the %zu byte limit", ws->max_message);
            return WS_MSG_ERROR;
          }
          if (frame.fin) {
            message->opcode = frame.opcode;
            buffer_append(&message->payload, frame.payload, frame.payload_len);
            buffer_consume(&ws->in, frame.frame_len);
            return WS_MSG_DATA;
          }
          ws->fragment_opcode = frame.opcode;
          buffer_append(&ws->fragment, frame.payload, frame.payload_len);
          buffer_consume(&ws->in, frame.frame_len);
          break;

        case WS_OPCODE_PING:
        case WS_OPCODE_PONG:
        case WS_OPCODE_CLOSE: {
          buffer_consume(&ws->in, frame.frame_len);
          ws_result result = handle_control(ws, &frame, message, err, err_len);
          if (result != WS_MSG_CLOSE) buffer_free(&message->payload);
          if (result == WS_MSG_PING || result == WS_MSG_PONG) continue;
          return result;
        }

        default:
          snprintf(err, err_len, "unknown opcode 0x%x", frame.opcode);
          return WS_MSG_ERROR;
      }
      continue;
    }

    if (parse_error != NULL) {
      snprintf(err, err_len, "%s", parse_error);
      return WS_MSG_ERROR;
    }

    /* Need more data */
    int remaining = -1;
    if (deadline != 0) {
      uint64_t now = now_ms();
      if (now >= deadline) return WS_MSG_NONE;
      remaining = (int)(deadline - now);
    } else if (timeout_ms == 0) {
      remaining = 0;
    }

    int rc;
    if (remaining == 0) {
      rc = read_more(ws, 0);
      if (rc == TC_IO_AGAIN) return WS_MSG_NONE;
    } else {
      rc = read_more(ws, remaining);
      if (rc == TC_IO_AGAIN) return WS_MSG_NONE;
    }

    if (rc == TC_IO_EOF) {
      snprintf(err, err_len, "connection closed by the server");
      return WS_MSG_EOF;
    }
    if (rc == TC_IO_ERROR) {
      snprintf(err, err_len, "network read error");
      return WS_MSG_ERROR;
    }
  }
}

/* ------------------------------------------------------------------------- */
/* frame writing                                                              */
/* ------------------------------------------------------------------------- */

static bool ws_write_frame(ws_conn *ws, int opcode, const void *data, size_t len) {
  unsigned char header[14];
  size_t header_len = 0;

  header[0] = (unsigned char)(0x80 | (opcode & 0x0f));
  if (len < 126) {
    header[1] = (unsigned char)(0x80 | len);
    header_len = 2;
  } else if (len < 65536) {
    header[1] = 0x80 | 126;
    header[2] = (unsigned char)(len >> 8);
    header[3] = (unsigned char)(len & 0xff);
    header_len = 4;
  } else {
    header[1] = 0x80 | 127;
    for (int i = 0; i < 8; i++) header[2 + i] = (unsigned char)((uint64_t)len >> (56 - 8 * i));
    header_len = 10;
  }

  unsigned char mask[4];
  if (!random_bytes(mask, sizeof(mask))) return false;
  memcpy(header + header_len, mask, 4);
  header_len += 4;

  if (!io_write_all(ws->io, header, header_len)) return false;

  if (len == 0) return true;

  /* Mask the payload in chunks so that large writes stay allocation free */
  unsigned char chunk[16384];
  const unsigned char *p = (const unsigned char *)data;
  size_t offset = 0;
  while (offset < len) {
    size_t n = len - offset;
    if (n > sizeof(chunk)) n = sizeof(chunk);
    for (size_t i = 0; i < n; i++) chunk[i] = p[offset + i] ^ mask[(offset + i) & 3];
    if (!io_write_all(ws->io, chunk, n)) return false;
    offset += n;
  }
  return true;
}

bool ws_send_binary(ws_conn *ws, const void *data, size_t len) {
  return ws_write_frame(ws, WS_OPCODE_BINARY, data, len);
}

bool ws_send_text(ws_conn *ws, const char *text) {
  return ws_write_frame(ws, WS_OPCODE_TEXT, text, text != NULL ? strlen(text) : 0);
}

bool ws_send_ping(ws_conn *ws, const void *data, size_t len) {
  if (len > 125) len = 125;
  return ws_write_frame(ws, WS_OPCODE_PING, data, len);
}

bool ws_send_pong(ws_conn *ws, const void *data, size_t len) {
  if (len > 125) len = 125;
  return ws_write_frame(ws, WS_OPCODE_PONG, data, len);
}

bool ws_send_close(ws_conn *ws, int code, const char *reason) {
  unsigned char payload[125];
  size_t len = 0;
  if (code > 0 && code != WS_CLOSE_NO_STATUS) {
    payload[0] = (unsigned char)((code >> 8) & 0xff);
    payload[1] = (unsigned char)(code & 0xff);
    len = 2;
    if (reason != NULL) {
      size_t reason_len = strlen(reason);
      if (reason_len > sizeof(payload) - 2) reason_len = sizeof(payload) - 2;
      memcpy(payload + 2, reason, reason_len);
      len += reason_len;
    }
  }
  ws->closing = true;
  return ws_write_frame(ws, WS_OPCODE_CLOSE, payload, len);
}
