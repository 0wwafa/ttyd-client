#ifndef TTYD_CLIENT_PROTOCOL_H
#define TTYD_CLIENT_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>

#include "buffer.h"

/* ttyd websocket protocol, see src/server.h of the ttyd repository.
 * Every message is a single binary websocket frame whose first byte is the
 * command character. */

/* client -> server */
#define TC_CMD_INPUT '0'
#define TC_CMD_RESIZE_TERMINAL '1'
#define TC_CMD_PAUSE '2'
#define TC_CMD_RESUME '3'
#define TC_CMD_JSON_DATA '{'

/* server -> client */
#define TC_CMD_OUTPUT '0'
#define TC_CMD_SET_WINDOW_TITLE '1'
#define TC_CMD_SET_PREFERENCES '2'

typedef enum {
  TC_MSG_OUTPUT = 0,
  TC_MSG_WINDOW_TITLE,
  TC_MSG_PREFERENCES,
  TC_MSG_UNKNOWN,
} tc_msg_type;

typedef struct {
  tc_msg_type type;
  const char *data; /* payload after the command byte, not NUL terminated */
  size_t len;
} tc_msg;

/* Build the messages sent to the server. `out` is reset first. */
void protocol_init_message(buffer_t *out, const char *token, int columns, int rows);
void protocol_input(buffer_t *out, const void *data, size_t len);
void protocol_resize(buffer_t *out, int columns, int rows);
void protocol_pause(buffer_t *out);
void protocol_resume(buffer_t *out);

/* Classify an incoming message */
void protocol_parse(const char *payload, size_t len, tc_msg *msg);

#endif  // TTYD_CLIENT_PROTOCOL_H
