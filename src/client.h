#ifndef TTYD_CLIENT_CLIENT_H
#define TTYD_CLIENT_CLIENT_H

#include <stdbool.h>
#include <stddef.h>

#include "json.h"
#include "ws.h"

/* Action to take when stdin reaches EOF */
typedef enum {
  TC_EOF_ACTION_EOT = 0, /* send ^D (0x04) to the remote shell */
  TC_EOF_ACTION_QUIT,    /* disconnect from the server */
  TC_EOF_ACTION_IGNORE,  /* stop reading stdin, keep the session alive */
} tc_eof_action;

typedef struct {
  const char *url;

  int port;                 /* 0 = take the port from the url */
  const char *credential;   /* username:password for basic authentication */
  const ws_header *headers; /* extra request headers */
  size_t header_count;

  const char *url_args[64]; /* --url-arg key=value, forwarded in the ws query */
  size_t url_arg_count;

  bool readonly; /* do not forward stdin */
  bool no_reconnect;
  bool reconnect_on_eof;  /* also reconnect when the server ends the session cleanly */
  int reconnect_max;      /* maximum reconnect attempts, -1 = unlimited */
  int reconnect_delay_ms; /* initial backoff */
  int reconnect_max_delay_ms;
  int reconnect_reset_ms; /* a session that lived this long resets the backoff */

  int ping_interval_ms;   /* 0 disables client side pings */
  int io_timeout_ms;      /* socket read/write timeout */
  int connect_timeout_ms; /* tcp/tls/ws handshake timeout */

  int columns; /* initial size, 0 = use the local terminal size */
  int rows;
  bool no_resize;          /* never send RESIZE_TERMINAL messages */
  bool no_title;           /* never set the terminal window title */
  const char *title_fixed; /* --title fixed window title */

  int escape_char; /* local quit character, -1 disables it */

  int flow_limit;      /* bytes written before the flow control check */
  int flow_high_water; /* backlog size that triggers PAUSE */
  int flow_low_water;  /* backlog size that triggers RESUME */

  tc_eof_action eof_action;

  bool ssl;      /* force tls for urls without a scheme */
  bool insecure; /* do not verify the server certificate */
  const char *ca_file;
  const char *cert_file;
  const char *key_file;

  const char *proxy; /* proxy url, NULL = use the environment */
  bool no_proxy;

  json_value *local_prefs; /* --client-option overrides, may be NULL */

  bool quiet; /* suppress the notice messages */
} tc_options;

/* Run the client, returns the process exit code */
int client_run(tc_options *options);

#endif  // TTYD_CLIENT_CLIENT_H
