#include "protocol.h"

#include <stdio.h>
#include <string.h>

#include "json.h"

void protocol_init_message(buffer_t *out, const char *token, int columns, int rows) {
  buffer_reset(out);
  buffer_append_str(out, "{\"AuthToken\":");
  json_write_string(out, token != NULL ? token : "");
  if (columns > 0 && rows > 0) {
    buffer_appendf(out, ",\"columns\":%d,\"rows\":%d", columns, rows);
  }
  buffer_append_char(out, '}');
}

void protocol_input(buffer_t *out, const void *data, size_t len) {
  buffer_reset(out);
  buffer_append_char(out, TC_CMD_INPUT);
  buffer_append(out, data, len);
}

void protocol_resize(buffer_t *out, int columns, int rows) {
  buffer_reset(out);
  buffer_append_char(out, TC_CMD_RESIZE_TERMINAL);
  buffer_appendf(out, "{\"columns\":%d,\"rows\":%d}", columns, rows);
}

void protocol_pause(buffer_t *out) {
  buffer_reset(out);
  buffer_append_char(out, TC_CMD_PAUSE);
}

void protocol_resume(buffer_t *out) {
  buffer_reset(out);
  buffer_append_char(out, TC_CMD_RESUME);
}

void protocol_parse(const char *payload, size_t len, tc_msg *msg) {
  msg->data = NULL;
  msg->len = 0;
  msg->type = TC_MSG_UNKNOWN;

  if (payload == NULL || len == 0) return;

  msg->data = payload + 1;
  msg->len = len - 1;

  switch (payload[0]) {
    case TC_CMD_OUTPUT:
      msg->type = TC_MSG_OUTPUT;
      break;
    case TC_CMD_SET_WINDOW_TITLE:
      msg->type = TC_MSG_WINDOW_TITLE;
      break;
    case TC_CMD_SET_PREFERENCES:
      msg->type = TC_MSG_PREFERENCES;
      break;
    default:
      msg->type = TC_MSG_UNKNOWN;
      break;
  }
}
