#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat.h"
#include "client.h"
#include "json.h"
#include "log.h"
#include "net.h"
#include "utils.h"
#include "version.h"
#include "ws.h"

#define MAX_CLIENT_OPTIONS 64

static void print_help(void) {
  // clang-format off
  fprintf(stderr,
          "ttyd-client is a command line client for ttyd (share your terminal over the web)\n\n"
          "USAGE:\n"
          "    ttyd-client [options] <url>\n\n"
          "    The url can be ws://host:port/path, wss://host:port/path, http(s)://host:port/path\n"
          "    or a plain host[:port]. When the url points at the base path of a ttyd server\n"
          "    (eg: http://localhost:7681/my/base/), the /ws and /token endpoints are derived\n"
          "    from it automatically.\n\n"
          "VERSION:\n"
          "    %s\n\n"
          "OPTIONS:\n"
          "    -p, --port              Port to connect (default: from the url, else 7681)\n"
          "    -c, --credential        Credential for basic authentication (format: username:password)\n"
          "    -H, --auth-header       Extra HTTP header sent with every request (format: name=value), repeatable\n"
          "    -a, --url-arg           Client argument forwarded in the URL (format: key=value), repeatable,\n"
          "                            requires the server to run with --url-arg\n"
          "    -t, --client-option     Client option override (format: key=value), repeatable\n"
          "    -W, --writable          Allow sending keyboard input to the remote TTY (default)\n"
          "    -R, --readonly          Do not send keyboard input, only display the remote terminal\n"
          "    -e, --eof-action        What to do when stdin reaches EOF: eot, quit or ignore (default: eot)\n"
          "        --escape-char       Local character used to quit the client (default: ^], use `none` to disable)\n"
          "    -r, --reconnect         Maximum reconnect attempts (default: 5, use -1 for unlimited)\n"
          "        --no-reconnect      Do not reconnect on connection loss\n"
          "        --reconnect-on-eof  Also reconnect when the server ends the session (the remote\n"
          "                            command exited), like the browser client does\n"
          "        --reconnect-delay   Initial reconnect delay in milliseconds (default: 1000)\n"
          "        --reconnect-max-delay\n"
          "                            Maximum reconnect delay in milliseconds (default: 30000)\n"
          "        --connect-timeout   Timeout for connect/TLS/handshake in seconds (default: 15)\n"
          "    -P, --ping-interval     Websocket keepalive ping interval in seconds, 0 disables (default: 5)\n"
          "        --flow-limit        Bytes of output written before a flow control check (default: 100000)\n"
          "        --flow-high-water   Output backlog that pauses the server (default: 1048576)\n"
          "        --flow-low-water    Output backlog that resumes the server (default: 262144)\n"
          "        --cols, --rows      Initial terminal size (default: the local terminal size)\n"
          "        --no-resize         Never send terminal resize messages\n"
          "        --title             Fixed terminal window title\n"
          "        --no-title          Do not change the terminal window title\n"
          "    -S, --ssl               Use TLS (wss/https) for urls without a scheme\n"
          "    -k, --insecure          Do not verify the server certificate\n"
          "    -C, --ssl-cert          Client certificate file path (PEM)\n"
          "    -K, --ssl-key           Client key file path (PEM)\n"
          "    -A, --ssl-ca            CA bundle used to verify the server (PEM, replaces the system store)\n"
          "    -x, --proxy             HTTP proxy used for the connection (default: $https_proxy/$http_proxy/$all_proxy)\n"
          "        --no-proxy          Never use a proxy\n"
          "    -q, --quiet             Only print errors\n"
          "    -d, --debug             Set log level (default: 6, 15 for debug output)\n"
          "    -v, --version           Print the version and exit\n"
          "    -h, --help              Print this text and exit\n\n"
          "EXAMPLES:\n"
          "    ttyd-client localhost:7681\n"
          "    ttyd-client ws://localhost:7681/ws\n"
          "    ttyd-client -c user:pass https://example.com/terminal/\n"
          "    ttyd-client --readonly wss://example.com/terminal/ws\n\n"
          "Press Ctrl-] to detach from the session without killing the remote command.\n"
          "Visit https://github.com/tsl0922/ttyd to get more information about ttyd.\n",
          TTYD_CLIENT_VERSION);
  // clang-format on
}

static void print_version(void) {
  printf("ttyd-client version %s\n", TTYD_CLIENT_VERSION);
  printf("TLS support: enabled (OpenSSL)\n");
}

static int parse_int_arg(const char *name, const char *value, long min, long max) {
  long parsed = 0;
  if (!parse_long(value, &parsed) || parsed < min || parsed > max) {
    fprintf(stderr, "ttyd-client: invalid value for %s: %s\n", name, value);
    exit(EXIT_FAILURE);
  }
  return (int)parsed;
}

/* Split "name=value" into two allocated strings, returns false when the '=' is missing */
static bool split_key_value(const char *input, char **name, char **value) {
  const char *eq = strchr(input, '=');
  if (eq == NULL || eq == input) return false;
  *name = xstrndup(input, (size_t)(eq - input));
  *value = xstrdup(eq + 1);
  return true;
}

int main(int argc, char **argv) {
  if (argc == 1) {
    print_help();
    return EXIT_FAILURE;
  }

  tc_options opts;
  memset(&opts, 0, sizeof(opts));
  opts.reconnect_max = 5;
  opts.reconnect_delay_ms = 1000;
  opts.reconnect_max_delay_ms = 30000;
  opts.reconnect_reset_ms = 5000;
  opts.ping_interval_ms = 5000;
  opts.io_timeout_ms = 60000;
  opts.connect_timeout_ms = 15000;
  opts.escape_char = 0x1d; /* Ctrl-] */
  opts.flow_limit = 100000;
  opts.flow_high_water = 1024 * 1024;
  opts.flow_low_water = 256 * 1024;
  opts.eof_action = TC_EOF_ACTION_EOT;

  ws_header headers[32];
  size_t header_count = 0;
  const char *url_args[MAX_CLIENT_OPTIONS];
  size_t url_arg_count = 0;
  json_value *local_prefs = NULL;
  const char *eof_action = NULL;
  const char *escape_char = NULL;
  const char *proxy_url = NULL;
  bool no_proxy = false;

  static const struct option long_options[] = {{"port", required_argument, NULL, 'p'},
                                               {"credential", required_argument, NULL, 'c'},
                                               {"auth-header", required_argument, NULL, 'H'},
                                               {"url-arg", required_argument, NULL, 'a'},
                                               {"client-option", required_argument, NULL, 't'},
                                               {"writable", no_argument, NULL, 'W'},
                                               {"readonly", no_argument, NULL, 'R'},
                                               {"eof-action", required_argument, NULL, 'e'},
                                               {"escape-char", required_argument, NULL, 1},
                                               {"reconnect", required_argument, NULL, 'r'},
                                               {"no-reconnect", no_argument, NULL, 2},
                                               {"reconnect-on-eof", no_argument, NULL, 15},
                                               {"reconnect-delay", required_argument, NULL, 3},
                                               {"reconnect-max-delay", required_argument, NULL, 4},
                                               {"connect-timeout", required_argument, NULL, 5},
                                               {"ping-interval", required_argument, NULL, 'P'},
                                               {"flow-limit", required_argument, NULL, 6},
                                               {"flow-high-water", required_argument, NULL, 7},
                                               {"flow-low-water", required_argument, NULL, 8},
                                               {"cols", required_argument, NULL, 9},
                                               {"rows", required_argument, NULL, 10},
                                               {"no-resize", no_argument, NULL, 11},
                                               {"title", required_argument, NULL, 12},
                                               {"no-title", no_argument, NULL, 13},
                                               {"ssl", no_argument, NULL, 'S'},
                                               {"insecure", no_argument, NULL, 'k'},
                                               {"ssl-cert", required_argument, NULL, 'C'},
                                               {"ssl-key", required_argument, NULL, 'K'},
                                               {"ssl-ca", required_argument, NULL, 'A'},
                                               {"proxy", required_argument, NULL, 'x'},
                                               {"no-proxy", no_argument, NULL, 14},
                                               {"quiet", no_argument, NULL, 'q'},
                                               {"debug", required_argument, NULL, 'd'},
                                               {"version", no_argument, NULL, 'v'},
                                               {"help", no_argument, NULL, 'h'},
                                               {NULL, 0, NULL, 0}};

  int c;
  while ((c = getopt_long(argc, argv, "p:c:H:a:t:WRe:r:P:SkC:K:A:x:qd:vh", long_options, NULL)) != -1) {
    switch (c) {
      case 'h':
        print_help();
        return EXIT_SUCCESS;
      case 'v':
        print_version();
        return EXIT_SUCCESS;
      case 'p':
        opts.port = parse_int_arg("port", optarg, 0, 65535);
        break;
      case 'c':
        if (strchr(optarg, ':') == NULL) {
          fprintf(stderr, "ttyd-client: invalid credential, format: username:password\n");
          return EXIT_FAILURE;
        }
        opts.credential = optarg;
        break;
      case 'H': {
        char *name = NULL, *value = NULL;
        if (!split_key_value(optarg, &name, &value)) {
          fprintf(stderr, "ttyd-client: invalid auth header: %s, format: name=value\n", optarg);
          return EXIT_FAILURE;
        }
        if (header_count < sizeof(headers) / sizeof(headers[0])) {
          headers[header_count].name = name;
          headers[header_count].value = value;
          header_count++;
        } else {
          fprintf(stderr, "ttyd-client: too many --auth-header options\n");
          return EXIT_FAILURE;
        }
        break;
      }
      case 'a':
        if (url_arg_count < MAX_CLIENT_OPTIONS) {
          url_args[url_arg_count++] = optarg;
        } else {
          fprintf(stderr, "ttyd-client: too many --url-arg options\n");
          return EXIT_FAILURE;
        }
        break;
      case 't': {
        char *name = NULL, *value = NULL;
        if (!split_key_value(optarg, &name, &value)) {
          fprintf(stderr, "ttyd-client: invalid client option: %s, format: key=value\n", optarg);
          return EXIT_FAILURE;
        }
        json_value *parsed = json_parse(value, strlen(value));
        if (parsed == NULL) {
          /* Not valid JSON: use the raw text as a string, like ttyd does for
           * its --client-option values. */
          parsed = xcalloc(1, sizeof(json_value));
          parsed->type = JSON_STRING;
          parsed->u.string = xstrdup(value);
        }
        if (local_prefs == NULL) {
          local_prefs = xcalloc(1, sizeof(json_value));
          local_prefs->type = JSON_OBJECT;
        }
        size_t n = local_prefs->u.object.count;
        local_prefs->u.object.keys = xrealloc(local_prefs->u.object.keys, (n + 1) * sizeof(char *));
        local_prefs->u.object.values = xrealloc(local_prefs->u.object.values, (n + 1) * sizeof(json_value *));
        local_prefs->u.object.keys[n] = name;
        local_prefs->u.object.values[n] = parsed;
        local_prefs->u.object.count = n + 1;
        free(value);
        break;
      }
      case 'W':
        opts.readonly = false;
        break;
      case 'R':
        opts.readonly = true;
        break;
      case 'e':
        eof_action = optarg;
        break;
      case 1: /* --escape-char */
        escape_char = optarg;
        break;
      case 'r':
        opts.reconnect_max = parse_int_arg("reconnect", optarg, -1, 100000);
        break;
      case 2:
        opts.no_reconnect = true;
        break;
      case 15:
        opts.reconnect_on_eof = true;
        break;
      case 3:
        opts.reconnect_delay_ms = parse_int_arg("reconnect-delay", optarg, 0, 3600000);
        break;
      case 4:
        opts.reconnect_max_delay_ms = parse_int_arg("reconnect-max-delay", optarg, 0, 3600000);
        break;
      case 5:
        opts.connect_timeout_ms = parse_int_arg("connect-timeout", optarg, 0, 86400) * 1000;
        break;
      case 'P':
        opts.ping_interval_ms = parse_int_arg("ping-interval", optarg, 0, 86400) * 1000;
        break;
      case 6:
        opts.flow_limit = parse_int_arg("flow-limit", optarg, 0, 1 << 30);
        break;
      case 7:
        opts.flow_high_water = parse_int_arg("flow-high-water", optarg, 0, 1 << 30);
        break;
      case 8:
        opts.flow_low_water = parse_int_arg("flow-low-water", optarg, 0, 1 << 30);
        break;
      case 9:
        opts.columns = parse_int_arg("cols", optarg, 0, 10000);
        break;
      case 10:
        opts.rows = parse_int_arg("rows", optarg, 0, 10000);
        break;
      case 11:
        opts.no_resize = true;
        break;
      case 12:
        opts.title_fixed = optarg;
        break;
      case 13:
        opts.no_title = true;
        break;
      case 'S':
        opts.ssl = true;
        break;
      case 'k':
        opts.insecure = true;
        break;
      case 'C':
        opts.cert_file = optarg;
        break;
      case 'K':
        opts.key_file = optarg;
        break;
      case 'A':
        opts.ca_file = optarg;
        break;
      case 'x':
        proxy_url = optarg;
        break;
      case 14:
        no_proxy = true;
        break;
      case 'q':
        opts.quiet = true;
        break;
      case 'd':
        log_set_level(parse_int_arg("debug", optarg, 0, 0x7fffffff));
        break;
      case '?':
      default:
        fprintf(stderr, "Try 'ttyd-client --help' for more information.\n");
        return EXIT_FAILURE;
    }
  }

  if (optind >= argc) {
    fprintf(stderr, "ttyd-client: missing url\n\n");
    print_help();
    return EXIT_FAILURE;
  }
  opts.url = argv[optind++];
  if (optind < argc) {
    fprintf(stderr, "ttyd-client: unexpected argument: %s\n", argv[optind]);
    return EXIT_FAILURE;
  }

  opts.headers = headers;
  opts.header_count = header_count;
  for (size_t i = 0; i < url_arg_count; i++) opts.url_args[i] = url_args[i];
  opts.url_arg_count = url_arg_count;
  opts.local_prefs = local_prefs;
  opts.proxy = proxy_url;
  opts.no_proxy = no_proxy;

  if (eof_action != NULL) {
    if (!strcmp(eof_action, "eot")) {
      opts.eof_action = TC_EOF_ACTION_EOT;
    } else if (!strcmp(eof_action, "quit")) {
      opts.eof_action = TC_EOF_ACTION_QUIT;
    } else if (!strcmp(eof_action, "ignore")) {
      opts.eof_action = TC_EOF_ACTION_IGNORE;
    } else {
      fprintf(stderr, "ttyd-client: invalid --eof-action: %s (expected eot, quit or ignore)\n", eof_action);
      return EXIT_FAILURE;
    }
  }

  if (escape_char != NULL) {
    if (!strcasecmp(escape_char, "none") || !strcmp(escape_char, "off")) {
      opts.escape_char = -1;
    } else if (!strcasecmp(escape_char, "ctrl-]") || !strcasecmp(escape_char, "ctrl-]")) {
      opts.escape_char = 0x1d;
    } else if (strlen(escape_char) == 1 && isprint((unsigned char)escape_char[0])) {
      opts.escape_char = (unsigned char)escape_char[0];
    } else if (startswith(escape_char, "0x")) {
      long value = 0;
      opts.escape_char = parse_long(escape_char + 2, &value) ? 0 : -1;
    } else {
      fprintf(stderr, "ttyd-client: invalid --escape-char: %s\n", escape_char);
      return EXIT_FAILURE;
    }
    if (opts.escape_char < 0 && strcasecmp(escape_char, "none") && strcmp(escape_char, "off")) {
      fprintf(stderr, "ttyd-client: invalid --escape-char: %s\n", escape_char);
      return EXIT_FAILURE;
    }
  }

  if (opts.flow_low_water > opts.flow_high_water) {
    fprintf(stderr, "ttyd-client: --flow-low-water must not be greater than --flow-high-water\n");
    return EXIT_FAILURE;
  }

  if (opts.quiet) log_set_level(TC_LLL_ERR);
  if (opts.quiet && log_get_level() == TC_DEFAULT_LOG_LEVEL) log_set_level(TC_LLL_ERR);

  int exit_code = client_run(&opts);
  json_free(local_prefs);
  for (size_t i = 0; i < header_count; i++) {
    free(headers[i].name);
    free(headers[i].value);
  }
  return exit_code;
}
