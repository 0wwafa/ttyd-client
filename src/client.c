#include "client.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "b64.h"
#include "http.h"
#include "io.h"
#include "json.h"
#include "log.h"
#include "net.h"
#include "protocol.h"
#include "term.h"
#include "url.h"
#include "utils.h"
#include "version.h"

#ifndef _WIN32
#include <fcntl.h>
#include <sys/select.h>
#include <unistd.h>
#endif

typedef enum {
  SESSION_QUIT = 0, /* the user asked to leave */
  SESSION_CLOSED,   /* the server closed the connection */
  SESSION_ERROR,    /* transport or protocol error */
} session_result;

typedef struct {
  tc_options *opts;
  tc_url url;
  tc_endpoints endpoints;
  tc_io io;
  ws_conn ws;
  tc_term term;
  tc_proxy proxy;

  buffer_t out; /* terminal output not yet written to stdout */
  buffer_t msg; /* scratch buffer for outgoing protocol messages */

  bool connected;
  bool paused; /* a PAUSE message was sent to the server */
  bool stdin_closed;
  bool eof_pending; /* ^D or a quit queued, delivered a moment later */
  bool eof_pending_quit;
  bool want_quit;
  bool fatal_error; /* do not retry, eg: authentication failure */
  bool title_fixed;
  char *title_fixed_value;

  char token[1024];
  int columns;
  int rows;
  int last_term_columns; /* last terminal size seen by the poller */
  int last_term_rows;
  bool term_size_valid;
  int close_code;
  char disconnect_reason[256];

  uint64_t last_ping;
  uint64_t eof_send_at; /* when to deliver a deferred ^D after stdin EOF */
  uint64_t last_size_check;
  uint64_t bytes_since_check;
} tc_session;

static tc_session *active_session = NULL;

/* ------------------------------------------------------------------------- */
/* helpers                                                                    */
/* ------------------------------------------------------------------------- */

static char *build_basic_auth(const char *credential) {
  char *encoded = base64_encode_alloc(credential, strlen(credential));
  size_t len = strlen(encoded) + 8;
  char *value = xmalloc(len);
  snprintf(value, len, "Basic %s", encoded);
  free(encoded);
  return value;
}

/* Read available bytes from stdout, returns bytes written, 0 when the output
 * would block or -1 on error. */
static int raw_stdout_write(const void *data, size_t len) {
#ifdef _WIN32
  HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD written = 0;
  if (!WriteFile(handle, data, (DWORD)len, &written, NULL)) return -1;
  return (int)written;
#else
  for (;;) {
    ssize_t n = write(STDOUT_FILENO, data, len);
    if (n >= 0) return (int)n;
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
  }
#endif
}

static void set_stdout_nonblocking(bool enable) {
#ifndef _WIN32
  static int saved_flags = -1;
  int flags = fcntl(STDOUT_FILENO, F_GETFL, 0);
  if (flags < 0) return;
  if (saved_flags < 0) saved_flags = flags;
  if (enable) {
    fcntl(STDOUT_FILENO, F_SETFL, flags | O_NONBLOCK);
  } else {
    fcntl(STDOUT_FILENO, F_SETFL, saved_flags);
  }
#else
  (void)enable;
#endif
}

static void session_flush_output(tc_session *s);

static void session_cleanup(void) {
  tc_session *s = active_session;
  if (s == NULL) return;
  term_restore(&s->term);
  log_set_crlf(false);
  set_stdout_nonblocking(false);
  io_close(&s->io);
  active_session = NULL;
}

static void session_send_pause(tc_session *s) {
  if (s->paused) return;
  protocol_pause(&s->msg);
  if (ws_send_binary(&s->ws, s->msg.data, s->msg.len)) {
    s->paused = true;
    log_debug("sent PAUSE (output backlog: %zu bytes)\n", s->out.len);
  }
}

static void session_send_resume(tc_session *s) {
  if (!s->paused) return;
  protocol_resume(&s->msg);
  if (ws_send_binary(&s->ws, s->msg.data, s->msg.len)) {
    s->paused = false;
    log_debug("sent RESUME\n");
  }
}

static void session_push_output(tc_session *s, const void *data, size_t len) {
  buffer_append(&s->out, data, len);
  s->bytes_since_check += len;

  if (s->bytes_since_check >= (uint64_t)s->opts->flow_limit && s->out.len >= (size_t)s->opts->flow_high_water) {
    session_send_pause(s);
    s->bytes_since_check = 0;
  }
  session_flush_output(s);
}

static void session_flush_output(tc_session *s) {
  while (s->out.len > 0) {
    int n = raw_stdout_write(s->out.data, s->out.len);
    if (n < 0) {
      log_debug("cannot write to stdout\n");
      s->want_quit = true;
      return;
    }
    if (n == 0) break; /* would block, wait for writability */
    buffer_consume(&s->out, (size_t)n);
  }

  if (s->paused && s->out.len <= (size_t)s->opts->flow_low_water) {
    session_send_resume(s);
    s->bytes_since_check = 0;
  }
}

static void session_send_resize(tc_session *s) {
  if (s->opts->no_resize) return;
  protocol_resize(&s->msg, s->columns, s->rows);
  if (!ws_send_binary(&s->ws, s->msg.data, s->msg.len)) {
    snprintf(s->disconnect_reason, sizeof(s->disconnect_reason), "cannot send terminal resize");
  } else {
    log_debug("sent resize: %dx%d\n", s->columns, s->rows);
  }
}

/* ------------------------------------------------------------------------- */
/* preferences                                                                */
/* ------------------------------------------------------------------------- */

static void apply_preferences(tc_session *s, json_value *prefs) {
  if (prefs == NULL || prefs->type != JSON_OBJECT) return;

  for (size_t i = 0; i < prefs->u.object.count; i++) {
    const char *key = prefs->u.object.keys[i];
    json_value *value = prefs->u.object.values[i];

    if (!strcmp(key, "titleFixed")) {
      if (value->type == JSON_STRING && value->u.string[0] != '\0') {
        free(s->title_fixed_value);
        s->title_fixed_value = xstrdup(value->u.string);
        s->title_fixed = true;
        term_set_title(&s->term, value->u.string);
        log_info("fixed window title: %s\n", value->u.string);
      }
    } else if (!strcmp(key, "disableReconnect") || !strcmp(key, "closeOnDisconnect")) {
      if (value->type == JSON_BOOL && value->u.boolean) {
        s->opts->no_reconnect = true;
        log_info("%s enabled, reconnect disabled\n", key);
      }
    } else if (!strcmp(key, "disableStdin")) {
      if (value->type == JSON_BOOL && value->u.boolean) {
        s->opts->readonly = true;
        log_info("disableStdin enabled, input disabled\n");
      }
    } else if (!strcmp(key, "disableTitle")) {
      if (value->type == JSON_BOOL && value->u.boolean) {
        s->opts->no_title = true;
        term_set_title_enabled(&s->term, false);
      }
    } else {
      buffer_t dump;
      buffer_init(&dump);
      json_write(&dump, value);
      log_debug("client option: %s=%s\n", key, dump.data != NULL ? dump.data : "");
      buffer_free(&dump);
    }
  }
}

static void handle_preferences(tc_session *s, const tc_msg *msg) {
  json_value *prefs = json_parse(msg->data, msg->len);
  if (prefs == NULL) {
    log_warn("cannot parse the server preferences: %.*s\n", (int)msg->len, msg->data);
    return;
  }
  /* Local --client-option values take precedence, like url query options do
   * in the browser client. */
  apply_preferences(s, prefs);
  if (s->opts->local_prefs != NULL) apply_preferences(s, s->opts->local_prefs);
  json_free(prefs);
}

/* ------------------------------------------------------------------------- */
/* connection                                                                 */
/* ------------------------------------------------------------------------- */

/* Build the proxy settings from --proxy or the usual environment variables */
static void session_proxy_init(tc_options *opts, tc_proxy *proxy) {
  memset(proxy, 0, sizeof(*proxy));
  if (opts->no_proxy) return;

  const char *value = opts->proxy;
  const char *from_env = NULL;
  if (value == NULL) {
    static const char *names[] = {"https_proxy", "HTTPS_PROXY", "http_proxy", "HTTP_PROXY", "all_proxy", "ALL_PROXY"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
      const char *env = getenv(names[i]);
      if (env != NULL && env[0] != '\0') {
        from_env = names[i];
        value = env;
        break;
      }
    }
  }
  if (value == NULL || value[0] == '\0') return;

  char *tmp = NULL;
  if (strstr(value, "://") == NULL) {
    size_t len = strlen(value) + 8;
    tmp = xmalloc(len);
    snprintf(tmp, len, "http://%s", value);
    value = tmp;
  }

  tc_url url;
  char err[256] = "";
  if (!url_parse(value, false, &url, err, sizeof(err))) {
    log_warn("ignoring invalid proxy setting%s%s: %s\n", from_env ? " from " : "", from_env ? from_env : "", err);
    free(tmp);
    return;
  }
  if (strncmp(value, "socks", 5) == 0) {
    log_warn("SOCKS proxies are not supported, connecting directly\n");
    url_free(&url);
    free(tmp);
    return;
  }
  if (url.tls) {
    log_warn("HTTPS proxies are not supported, connecting directly\n");
    url_free(&url);
    free(tmp);
    return;
  }

  proxy->enabled = true;
  proxy->host = xstrdup(url.host);
  proxy->port = url.port;
  if (url.user != NULL) {
    proxy->user = xstrdup(url.user);
    proxy->password = url.password != NULL ? xstrdup(url.password) : xstrdup("");
  }
  log_info("using proxy %s:%d%s%s\n", proxy->host, proxy->port, from_env ? " (from " : "", from_env ? from_env : "");
  url_free(&url);
  free(tmp);
}

static bool session_fetch_token(tc_session *s, char *err, size_t err_len) {
  /* Errors that will not go away by retrying */
#define FATAL() (s->fatal_error = true)
  s->token[0] = '\0';
  if (s->opts->credential == NULL) return true; /* no auth configured, nothing to fetch */

  ws_header headers[8];
  size_t header_count = 0;
  char *auth_value = build_basic_auth(s->opts->credential);
  headers[header_count].name = xstrdup("Authorization");
  headers[header_count++].value = auth_value; /* owned by the array below */
  for (size_t i = 0; i < s->opts->header_count && header_count < 8; i++) {
    headers[header_count].name = xstrdup(s->opts->headers[i].name);
    headers[header_count++].value = xstrdup(s->opts->headers[i].value);
  }

  tc_io io;
  tls_options tls_opts = {
      .verify = !s->opts->insecure,
      .ca_file = s->opts->ca_file,
      .cert_file = s->opts->cert_file,
      .key_file = s->opts->key_file,
      .server_name = NULL,
      .timeout_ms = s->opts->connect_timeout_ms,
  };
  bool ok = false;
  if (!io_open(&io, &s->url, &tls_opts, &s->proxy, s->opts->connect_timeout_ms, err, err_len)) goto done;

  http_response response;
  if (http_request(&io, "GET", s->endpoints.token, s->url.host, s->url.port, headers, header_count, &response, err,
                   err_len)) {
    if (response.status == 200) {
      json_value *json = json_parse(response.body.data, response.body.len);
      const char *token = json_get_string(json, "token", "");
      snprintf(s->token, sizeof(s->token), "%s", token != NULL ? token : "");
      json_free(json);
      log_debug("fetched token from %s\n", s->endpoints.token);
      ok = true;
    } else if (response.status == 401) {
      FATAL();
      snprintf(err, err_len, "authentication failed for %s (HTTP 401), check --credential", s->endpoints.token);
    } else if (response.status == 403) {
      FATAL();
      snprintf(err, err_len, "access to %s is forbidden (HTTP 403), check --auth-header", s->endpoints.token);
    } else if (response.status == 404) {
      FATAL();
      snprintf(err, err_len, "%s not found (HTTP 404), check the url path", s->endpoints.token);
    } else {
      snprintf(err, err_len, "unexpected response from %s: HTTP %d", s->endpoints.token, response.status);
    }
    http_response_free(&response);
  }
  io_close(&io);

done:
  for (size_t i = 0; i < header_count; i++) {
    free(headers[i].name);
    free(headers[i].value);
  }
  return ok;
}

static bool session_open_ws(tc_session *s, char *err, size_t err_len) {
  tls_options tls_opts = {
      .verify = !s->opts->insecure,
      .ca_file = s->opts->ca_file,
      .cert_file = s->opts->cert_file,
      .key_file = s->opts->key_file,
      .server_name = NULL,
      .timeout_ms = s->opts->connect_timeout_ms,
  };
  if (!io_open(&s->io, &s->url, &tls_opts, &s->proxy, s->opts->connect_timeout_ms, err, err_len)) return false;

  /* Build the request target, appending --url-arg values */
  char *query = s->url.query != NULL ? xstrdup(s->url.query) : NULL;
  for (size_t i = 0; i < s->opts->url_arg_count; i++) {
    const char *arg = s->opts->url_args[i];
    const char *eq = strchr(arg, '=');
    if (eq != NULL) {
      char *key = xstrndup(arg, (size_t)(eq - arg));
      url_append_query(&query, key, eq + 1);
      free(key);
    } else {
      url_append_query(&query, "arg", arg);
    }
  }

  buffer_t target;
  buffer_init(&target);
  buffer_append_str(&target, s->endpoints.ws);
  if (query != NULL && query[0] != '\0') {
    buffer_append_char(&target, '?');
    buffer_append_str(&target, query);
  }

  ws_header headers[16];
  size_t header_count = 0;
  char *auth_value = NULL;
  if (s->opts->credential != NULL) {
    auth_value = build_basic_auth(s->opts->credential);
    headers[header_count].name = xstrdup("Authorization");
    headers[header_count++].value = auth_value;
  }
  for (size_t i = 0; i < s->opts->header_count && header_count < 16; i++) {
    headers[header_count].name = xstrdup(s->opts->headers[i].name);
    headers[header_count++].value = xstrdup(s->opts->headers[i].value);
  }

  ws_init(&s->ws, &s->io);
  bool ok = ws_handshake(&s->ws, s->url.host, s->url.port, target.data, headers, header_count, err, err_len);
  /* A rejected upgrade (auth, wrong path) is not going to succeed on retry */
  if (!ok && (strstr(err, "HTTP 401") != NULL || strstr(err, "HTTP 403") != NULL || strstr(err, "HTTP 404") != NULL ||
              strstr(err, "unexpected subprotocol") != NULL)) {
    s->fatal_error = true;
  }

  for (size_t i = 0; i < header_count; i++) {
    free(headers[i].name);
    free(headers[i].value);
  }
  buffer_free(&target);
  free(query);

  if (!ok) {
    ws_free(&s->ws);
    io_close(&s->io);
    return false;
  }
  return true;
}

static bool session_connect(tc_session *s, char *err, size_t err_len) {
  s->fatal_error = false;
  char token_err[512];
  if (!session_fetch_token(s, token_err, sizeof(token_err))) {
    snprintf(err, err_len, "%s", token_err);
    return false;
  }

  if (!session_open_ws(s, err, err_len)) return false;

  if (s->opts->columns > 0 && s->opts->rows > 0) {
    s->columns = s->opts->columns;
    s->rows = s->opts->rows;
  } else {
    term_get_size(&s->term, &s->columns, &s->rows);
  }

  protocol_init_message(&s->msg, s->token, s->columns, s->rows);
  log_debug("sending initial message: %s\n", s->msg.data);
  if (!ws_send_binary(&s->ws, s->msg.data, s->msg.len)) {
    snprintf(err, err_len, "cannot send the initial message");
    ws_free(&s->ws);
    io_close(&s->io);
    return false;
  }

  s->connected = true;
  s->paused = false;
  s->stdin_closed = false;
  s->close_code = 0;
  s->disconnect_reason[0] = '\0';
  s->last_ping = now_ms();
  s->eof_pending = false;
  s->eof_pending_quit = false;
  s->last_size_check = 0;
  s->term_size_valid = false;
  s->bytes_since_check = 0;

  if (s->opts->escape_char >= 0) {
    log_debug("press %s to quit the local client\n", s->opts->escape_char > 32 ? "the escape key" : "Ctrl-]");
  }
  return true;
}

/* ------------------------------------------------------------------------- */
/* message handling                                                           */
/* ------------------------------------------------------------------------- */

static void handle_server_message(tc_session *s, ws_message *message) {
  if (message->opcode != WS_OPCODE_BINARY && message->opcode != WS_OPCODE_TEXT) return;

  tc_msg msg;
  protocol_parse(message->payload.data, message->payload.len, &msg);
  switch (msg.type) {
    case TC_MSG_OUTPUT:
      if (msg.len > 0) session_push_output(s, msg.data, msg.len);
      break;
    case TC_MSG_WINDOW_TITLE: {
      if (s->opts->no_title) break;
      size_t len = msg.len;
      char *title = xmalloc(len + 1);
      memcpy(title, msg.data, len);
      title[len] = '\0';
      log_debug("window title: %s\n", title);
      if (!s->title_fixed) term_set_title(&s->term, title);
      free(title);
      break;
    }
    case TC_MSG_PREFERENCES:
      handle_preferences(s, &msg);
      break;
    default:
      log_debug("ignored unknown message type: %c\n", message->payload.len > 0 ? message->payload.data[0] : '?');
      break;
  }
}

/* ------------------------------------------------------------------------- */
/* main loop                                                                  */
/* ------------------------------------------------------------------------- */

#ifndef _WIN32
static bool session_stdin_available(void) { return net_wait_readable(STDIN_FILENO, 0); }
#endif

static void session_handle_stdin(tc_session *s) {
  char buf[8192];
  for (;;) {
#ifdef _WIN32
    if (term_stdin_pending(&s->term) == 0) break;
#else
    if (!session_stdin_available()) break;
#endif
    int n = term_read_stdin(&s->term, buf, sizeof(buf));
    if (n == TC_STDIN_EOF) {
      s->stdin_closed = true;
      switch (s->opts->eof_action) {
        case TC_EOF_ACTION_EOT:
          /* Delay the ^D slightly: if the last thing we sent was a command,
           * the remote shell is still executing it and would discard the ^D
           * that lands in the same burst. */
          s->eof_pending = true;
          s->eof_send_at = now_ms() + 200;
          log_debug("stdin closed, ^D queued\n");
          break;
        case TC_EOF_ACTION_QUIT:
          /* Let the tail of the remote output arrive before leaving */
          s->eof_pending = true;
          s->eof_pending_quit = true;
          s->eof_send_at = now_ms() + 200;
          log_debug("stdin closed, quit queued\n");
          break;
        case TC_EOF_ACTION_IGNORE:
        default:
          log_debug("stdin closed, ignoring\n");
          break;
      }
      return;
    }
    if (n == TC_STDIN_ERROR) {
      s->stdin_closed = true;
      return;
    }
    if (n == 0) break;

    if (s->opts->readonly) continue;

    /* Local escape character: leave the session without disturbing the remote */
    size_t send_len = (size_t)n;
    if (s->opts->escape_char >= 0) {
      for (size_t i = 0; i < (size_t)n; i++) {
        if ((unsigned char)buf[i] == (unsigned char)s->opts->escape_char) {
          send_len = i;
          s->want_quit = true;
          log_debug("escape character pressed, closing the session\n");
          break;
        }
      }
    }

    if (send_len == 0) return;
    protocol_input(&s->msg, buf, send_len);
    if (!ws_send_binary(&s->ws, s->msg.data, s->msg.len)) {
      snprintf(s->disconnect_reason, sizeof(s->disconnect_reason), "cannot send input to the server");
      return;
    }
    if (s->want_quit) return;
  }
}

static session_result session_run(tc_session *s) {
  for (;;) {
    if (s->want_quit || term_should_quit(&s->term)) {
      s->want_quit = true;
      return SESSION_QUIT;
    }

    /* Terminal size changes: polled, because SIGWINCH is not delivered by
     * every terminal multiplexer and does not exist on Windows. */
    uint64_t now = now_ms();
    if (now - s->last_size_check >= 100) {
      s->last_size_check = now;
      int columns = 0, rows = 0;
      term_get_size(&s->term, &columns, &rows);
      if (!s->term_size_valid) {
        /* The first sample only establishes a baseline, so that an explicit
         * --cols/--rows is not immediately overridden. */
        s->term_size_valid = true;
        s->last_term_columns = columns;
        s->last_term_rows = rows;
      } else if (columns != s->last_term_columns || rows != s->last_term_rows) {
        s->last_term_columns = columns;
        s->last_term_rows = rows;
        s->columns = columns;
        s->rows = rows;
        session_send_resize(s);
      }
    }

    fd_set rfds, wfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    /* Safety valve: if the local terminal cannot keep up, stop reading from the
     * websocket (TCP backpressure) instead of buffering without limit. The
     * server normally stops sending long before this, because of PAUSE. */
    size_t backlog_cap = (size_t)s->opts->flow_high_water * 16;
    bool read_socket = backlog_cap == 0 || s->out.len <= backlog_cap;
    if (read_socket) FD_SET(s->io.fd, &rfds);
#ifndef _WIN32
    socket_t max_fd = s->io.fd;
#endif

    bool watch_stdin = !s->stdin_closed && !s->opts->readonly;
#ifndef _WIN32
    if (watch_stdin) {
      FD_SET(STDIN_FILENO, &rfds);
      if (STDIN_FILENO > max_fd) max_fd = STDIN_FILENO;
    }
    int resize_fd = term_resize_fd(&s->term);
    if (resize_fd >= 0) {
      FD_SET(resize_fd, &rfds);
      if (resize_fd > max_fd) max_fd = resize_fd;
    }
#endif
#ifndef _WIN32
    bool flush_pending = s->out.len > 0;
    if (flush_pending) {
      FD_SET(STDOUT_FILENO, &wfds);
      if (STDOUT_FILENO > max_fd) max_fd = STDOUT_FILENO;
    }
#endif

    int timeout = 1000;
    if (s->opts->ping_interval_ms > 0) {
      uint64_t elapsed = now_ms() - s->last_ping;
      int remaining = s->opts->ping_interval_ms - (int)elapsed;
      if (remaining < 0) remaining = 0;
      if (remaining < timeout) timeout = remaining;
    }
#ifdef _WIN32
    /* Console input is pumped by a thread, poll the queue regularly */
    if (watch_stdin && timeout > 50) timeout = 50;
#else
    if (watch_stdin) {
      /* Nothing else needed: select() wakes up on input */
    }
#endif
    if (s->out.len > 0 && timeout > 100) timeout = 100;

    struct timeval tv;
    tv.tv_sec = timeout / 1000;
    tv.tv_usec = (timeout % 1000) * 1000;
#ifdef _WIN32
    int rc = select(0, &rfds, &wfds, NULL, &tv);
#else
    int rc = select((int)(max_fd + 1), &rfds, &wfds, NULL, &tv);
#endif
    if (rc < 0) {
#ifndef _WIN32
      if (errno == EINTR) continue;
#endif
      snprintf(s->disconnect_reason, sizeof(s->disconnect_reason), "select() failed");
      return SESSION_ERROR;
    }

#ifndef _WIN32
    if (resize_fd >= 0 && FD_ISSET(resize_fd, &rfds)) term_drain_resize(&s->term);
    if (watch_stdin && FD_ISSET(STDIN_FILENO, &rfds)) session_handle_stdin(s);
    if (s->out.len > 0 && FD_ISSET(STDOUT_FILENO, &wfds)) session_flush_output(s);
#else
    if (watch_stdin) session_handle_stdin(s);
    if (s->out.len > 0) session_flush_output(s);
#endif

    if (s->want_quit) return SESSION_QUIT;

    if (read_socket && (FD_ISSET(s->io.fd, &rfds) || io_has_buffered(&s->io))) {
      /* Drain everything that is buffered before going back to select() */
      for (int i = 0; i < 256; i++) {
        ws_message message;
        char err[256] = "";
        ws_result result = ws_read_message(&s->ws, &message, 0, err, sizeof(err));
        if (result == WS_MSG_NONE) break;
        if (result == WS_MSG_DATA) {
          handle_server_message(s, &message);
          ws_message_free(&message);
          continue;
        }
        if (result == WS_MSG_PING || result == WS_MSG_PONG) {
          ws_message_free(&message);
          continue;
        }
        if (result == WS_MSG_CLOSE) {
          s->close_code = s->ws.close_code > 0 ? s->ws.close_code : WS_CLOSE_NORMAL;
          snprintf(s->disconnect_reason, sizeof(s->disconnect_reason), "connection closed by the server");
          ws_message_free(&message);
          session_flush_output(s);
          ws_send_close(&s->ws, WS_CLOSE_NORMAL, "bye");
          return SESSION_CLOSED;
        }
        if (result == WS_MSG_EOF) {
          /* ttyd closes the websocket when the remote command exits, without
           * necessarily sending a closing frame: that is a normal end of the
           * session, not a transport failure. */
          s->close_code = WS_CLOSE_NORMAL;
          snprintf(s->disconnect_reason, sizeof(s->disconnect_reason), "session ended by the server");
          session_flush_output(s);
          return SESSION_CLOSED;
        }
        /* WS_MSG_ERROR */
        s->close_code = WS_CLOSE_ABNORMAL;
        snprintf(s->disconnect_reason, sizeof(s->disconnect_reason), "%s", err[0] ? err : "websocket error");
        session_flush_output(s);
        return SESSION_ERROR;
      }
    }

    /* Deliver the deferred ^D once the remote shell is idle again */
    if (s->eof_pending && now_ms() >= s->eof_send_at) {
      const char eot = 0x04;
      s->eof_pending = false;
      if (s->eof_pending_quit) {
        s->eof_pending_quit = false;
        log_debug("stdin closed, quitting\n");
        s->want_quit = true;
        session_flush_output(s);
        return SESSION_QUIT;
      }
      protocol_input(&s->msg, &eot, 1);
      if (!ws_send_binary(&s->ws, s->msg.data, s->msg.len)) {
        snprintf(s->disconnect_reason, sizeof(s->disconnect_reason), "cannot send ^D");
        return SESSION_ERROR;
      }
      log_debug("stdin closed, sent ^D\n");
    }

    /* Keepalive */
    if (s->opts->ping_interval_ms > 0) {
      now = now_ms();
      if (now - s->last_ping >= (uint64_t)s->opts->ping_interval_ms) {
        if (!ws_send_ping(&s->ws, NULL, 0)) {
          snprintf(s->disconnect_reason, sizeof(s->disconnect_reason), "cannot send keepalive ping");
          return SESSION_ERROR;
        }
        s->last_ping = now;
        log_debug("sent keepalive ping\n");
      }
    }
  }
}

/* ------------------------------------------------------------------------- */
/* reconnect logic                                                            */
/* ------------------------------------------------------------------------- */

static bool sleep_with_abort(tc_session *s, int ms) {
  uint64_t deadline = now_ms() + (uint64_t)ms;
  while (now_ms() < deadline) {
    if (term_should_quit(&s->term)) return false;
    sleep_ms(50);
  }
  return true;
}

static void session_close(tc_session *s) {
  if (s->want_quit && s->connected) {
    ws_send_close(&s->ws, WS_CLOSE_NORMAL, "bye");
  }
  ws_free(&s->ws);
  io_close(&s->io);
  s->connected = false;
}

int client_run(tc_options *opts) {
  tc_session session;
  memset(&session, 0, sizeof(session));
  session.opts = opts;
  buffer_init(&session.out);
  buffer_init(&session.msg);

  char err[512] = "";
  if (!url_parse(opts->url, opts->ssl, &session.url, err, sizeof(err))) {
    log_err("%s\n", err);
    return EXIT_FAILURE;
  }
  if (opts->port > 0) session.url.port = opts->port;
  url_endpoints(&session.url, &session.endpoints);
  session_proxy_init(opts, &session.proxy);

  if (opts->credential == NULL && session.url.user != NULL) {
    /* Allow ws://user:password@host/url */
    size_t len = strlen(session.url.user) + (session.url.password ? strlen(session.url.password) : 0) + 2;
    char *credential = xmalloc(len);
    snprintf(credential, len, "%s:%s", session.url.user, session.url.password != NULL ? session.url.password : "");
    opts->credential = credential;
  }

  log_info("connecting to %s://%s:%d%s\n", session.url.tls ? "wss" : "ws", session.url.host, session.url.port,
           session.endpoints.ws);

  active_session = &session;
  atexit(session_cleanup);

  if (!net_init()) {
    log_err("cannot initialize the network stack\n");
    return EXIT_FAILURE;
  }

  if (!term_init(&session.term, err, sizeof(err))) {
    log_err("%s\n", err);
    return EXIT_FAILURE;
  }
  term_set_title_enabled(&session.term, !opts->no_title);
  if (opts->title_fixed != NULL) {
    session.title_fixed = true;
    session.title_fixed_value = xstrdup(opts->title_fixed);
    term_set_title(&session.term, opts->title_fixed);
  }

  if (!opts->readonly) {
    if (!term_enter_raw(&session.term, err, sizeof(err))) {
      log_err("%s\n", err);
      return EXIT_FAILURE;
    }
    /* Raw mode disables the LF -> CRLF translation, keep the log readable */
    log_set_crlf(session.term.raw_active);
  }
  term_install_signals(&session.term);

  int attempts = 0;
  int exit_code = EXIT_SUCCESS;

  for (;;) {
    if (term_should_quit(&session.term)) {
      session.want_quit = true;
      break;
    }

    if (!session_connect(&session, err, sizeof(err))) {
      log_err("%s\n", err);
      bool can_retry =
          !opts->no_reconnect && !session.fatal_error && (opts->reconnect_max < 0 || attempts < opts->reconnect_max);
      if (!can_retry) {
        exit_code = EXIT_FAILURE;
        break;
      }
      attempts++;
      int delay = opts->reconnect_delay_ms;
      for (int i = 1; i < attempts && delay < opts->reconnect_max_delay_ms; i++) delay *= 2;
      if (delay > opts->reconnect_max_delay_ms) delay = opts->reconnect_max_delay_ms;
      if (delay > 0) {
        if (opts->reconnect_max > 0) {
          log_notice("reconnecting in %d ms (attempt %d/%d)\n", delay, attempts, opts->reconnect_max);
        } else {
          log_notice("reconnecting in %d ms (attempt %d)\n", delay, attempts);
        }
        if (!sleep_with_abort(&session, delay)) {
          session.want_quit = true;
          break;
        }
      }
      continue;
    }

    log_notice("connected to %s:%d\n", session.url.host, session.url.port);

    set_stdout_nonblocking(true);
    uint64_t connected_at = now_ms();
    session_result result = session_run(&session);
    set_stdout_nonblocking(false);
    session_flush_output(&session);

    if (session.term.stdin_is_tty && session.term.stdout_is_tty) {
      /* Leave a clean line behind after the remote output */
      raw_stdout_write("\r\n", 2);
    }

    session_close(&session);

    if (result == SESSION_QUIT) {
      exit_code = EXIT_SUCCESS;
      break;
    }

    bool normal = session.close_code == WS_CLOSE_NORMAL;
    if (session.disconnect_reason[0] != '\0') {
      log_notice("%s (code %d)\n", session.disconnect_reason, session.close_code);
    }

    /* A session that stayed up for a while resets the backoff */
    if (now_ms() - connected_at >= (uint64_t)opts->reconnect_reset_ms) attempts = 0;

    /* The server ended the session on purpose (the remote command exited or
     * the client was kicked out): do not reconnect unless asked to. */
    if (result == SESSION_CLOSED && !opts->reconnect_on_eof) {
      exit_code = normal ? EXIT_SUCCESS : EXIT_FAILURE;
      break;
    }

    bool can_retry = !opts->no_reconnect && (opts->reconnect_max < 0 || attempts < opts->reconnect_max);
    if (!can_retry) {
      exit_code = normal ? EXIT_SUCCESS : EXIT_FAILURE;
      break;
    }

    attempts++;
    int delay = opts->reconnect_delay_ms;
    for (int i = 1; i < attempts && delay < opts->reconnect_max_delay_ms; i++) delay *= 2;
    if (delay > opts->reconnect_max_delay_ms) delay = opts->reconnect_max_delay_ms;
    if (opts->reconnect_max > 0) {
      log_notice("reconnecting in %d ms (attempt %d/%d)\n", delay, attempts, opts->reconnect_max);
    } else {
      log_notice("reconnecting in %d ms (attempt %d, ctrl-] to quit)\n", delay, attempts);
    }
    if (!sleep_with_abort(&session, delay)) {
      exit_code = EXIT_SUCCESS;
      break;
    }
  }

  session_cleanup();
  net_cleanup();
  buffer_free(&session.out);
  buffer_free(&session.msg);
  url_free(&session.url);
  url_endpoints_free(&session.endpoints);
  free(session.proxy.host);
  free(session.proxy.user);
  free(session.proxy.password);
  free(session.title_fixed_value);
  return exit_code;
}
