#ifndef TTYD_CLIENT_WS_H
#define TTYD_CLIENT_WS_H

#include <stdbool.h>
#include <stddef.h>

#include "buffer.h"
#include "header.h"
#include "io.h"

/* Minimal RFC 6455 websocket client, covering what the ttyd protocol needs:
 * binary messages, ping/pong and the closing handshake. */

#define WS_OPCODE_CONTINUATION 0x0
#define WS_OPCODE_TEXT 0x1
#define WS_OPCODE_BINARY 0x2
#define WS_OPCODE_CLOSE 0x8
#define WS_OPCODE_PING 0x9
#define WS_OPCODE_PONG 0xa

/* Close codes used by ttyd */
#define WS_CLOSE_NORMAL 1000
#define WS_CLOSE_GOING_AWAY 1001
#define WS_CLOSE_PROTOCOL_ERROR 1002
#define WS_CLOSE_UNSUPPORTED 1003
#define WS_CLOSE_NO_STATUS 1005
#define WS_CLOSE_ABNORMAL 1006
#define WS_CLOSE_INVALID_PAYLOAD 1007
#define WS_CLOSE_POLICY_VIOLATION 1008
#define WS_CLOSE_TOO_LARGE 1009
#define WS_CLOSE_INTERNAL_ERROR 1011

typedef tc_header ws_header;

typedef enum {
  WS_MSG_NONE = 0, /* nothing available yet (non blocking mode) */
  WS_MSG_DATA,     /* a complete data message is in `message` */
  WS_MSG_PING,
  WS_MSG_PONG,
  WS_MSG_CLOSE,
  WS_MSG_EOF,
  WS_MSG_ERROR,
} ws_result;

typedef struct {
  int opcode;
  buffer_t payload;
} ws_message;

typedef struct {
  tc_io *io;
  buffer_t in;       /* raw bytes received from the network */
  buffer_t fragment; /* assembled fragments of the current message */
  int fragment_opcode;
  bool closing;
  int close_code;
  size_t max_message;
} ws_conn;

/* Reset a connection struct */
void ws_init(ws_conn *ws, tc_io *io);

/* Free the buffers held by a connection (does not close the io) */
void ws_free(ws_conn *ws);

/* Perform the websocket upgrade handshake.
 * `target` is the request target, including the query string (eg: /ws?arg=foo).
 * On failure returns false and fills `err`. */
bool ws_handshake(ws_conn *ws, const char *host, int port, const char *target, const ws_header *headers,
                  size_t header_count, char *err, size_t err_len);

/* Read the next message. `timeout_ms` < 0 blocks forever, 0 returns
 * WS_MSG_NONE when no complete message is buffered. */
ws_result ws_read_message(ws_conn *ws, ws_message *message, int timeout_ms, char *err, size_t err_len);

/* Send helpers, return false when the socket write fails */
bool ws_send_binary(ws_conn *ws, const void *data, size_t len);
bool ws_send_text(ws_conn *ws, const char *text);
bool ws_send_ping(ws_conn *ws, const void *data, size_t len);
bool ws_send_pong(ws_conn *ws, const void *data, size_t len);
bool ws_send_close(ws_conn *ws, int code, const char *reason);

/* Free the payload of a message read with ws_read_message() */
void ws_message_free(ws_message *message);

#endif  // TTYD_CLIENT_WS_H
