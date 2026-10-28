#ifndef TTYD_CLIENT_HEADER_H
#define TTYD_CLIENT_HEADER_H

#include <stddef.h>

/* A single HTTP style header, shared by the websocket handshake and the
 * regular HTTP requests. */
typedef struct {
  char *name;
  char *value;
} tc_header;

#endif  // TTYD_CLIENT_HEADER_H
