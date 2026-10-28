ttyd-client 1 "October 2026" ttyd-client "User Manual"
==================================================

# NAME
  ttyd-client - connect a terminal to a ttyd server

# SYNOPSIS
  **ttyd-client** [options] \<url\>

# DESCRIPTION
  **ttyd-client** is a command line client for **ttyd**(1) (share your terminal over the web). It connects to the websocket endpoint of a ttyd server and attaches the local terminal to the remote session, so that a shared terminal can be used from a shell exactly like **ssh**(1), without a web browser.

  The client speaks the same protocol as the ttyd web frontend: it fetches the authentication token from the */token* endpoint, opens the */ws* websocket, sends the initial **JSON_DATA** message with the terminal size, and then exchanges **INPUT / OUTPUT / RESIZE_TERMINAL / PAUSE / RESUME** messages. Window titles and client preferences sent by the server are applied to the local terminal.

  Features:

  - Pure C, statically linked OpenSSL for TLS (wss/https)
  - Same protocol and client options as the ttyd web client
  - Basic authentication, auth proxy headers and URL arguments
  - Terminal resize forwarding, window title updates and flow control
  - Automatic reconnection with exponential backoff
  - HTTP proxy (CONNECT) and TLS (wss) support
  - Statically linkable, cross platform: Linux (glibc/musl), macOS, FreeBSD/OpenBSD, OpenWrt, Windows

# URLS
  The url argument accepts a plain `host` or `host:port` (default port 7681, **ws://** unless --ssl is given), a **ws://** or **wss://** url, or an **http(s)://** url pointing at the base path of a ttyd server. When the path ends with */ws* or */token* it is stripped to obtain the base path, so all of these are equivalent for a server on port 7681:

```bash
ttyd-client localhost:7681
ttyd-client ws://localhost:7681/ws
ttyd-client http://localhost:7681/
```

  For a server started with `ttyd -b /mounted/here`, use any of:

```bash
ttyd-client http://localhost:7681/mounted/here/
ttyd-client ws://localhost:7681/mounted/here/ws
```

# OPTIONS
  -p, --port \<port\>
      Port to connect to, overriding the port from the url.

  -c, --credential \<username:password\>
      Credential for basic authentication. The token is fetched from the */token* endpoint and sent in the **Authorization** header and in the initial **AuthToken** field, exactly like the web client does.

  -H, --auth-header \<name=value\>
      Extra HTTP header sent with the token request and the websocket handshake. Useful when ttyd runs behind a reverse proxy that authenticates requests (the --auth-header server option). Repeatable.

  -a, --url-arg \<key=value\>
      Append an argument to the websocket url. Requires the server to run with --url-arg, where it becomes an extra command line argument of the shared command. Repeatable. A value without `=` is sent as `arg=<value>`.

  -t, --client-option \<key=value\>
      Override a client preference sent by the server, for example `titleFixed=ttyd-client`. The value is parsed as JSON when possible, otherwise it is used as a string. Repeatable.

  -W, --writable
      Send keyboard input to the remote terminal (this is the default).

  -R, --readonly
      Do not send keyboard input, only display the remote terminal.

  -e, --eof-action \<eot|quit|ignore\>
      What to do when stdin reaches end of file: send a **^D** character (default), quit, or keep the session open.
      The **^D** (and the quit) is deferred by 200 ms so that it does not arrive in the middle of a command that was
      piped in on the last line, which keeps constructs such as `printf 'ls' | ttyd-client host` working.

  --escape-char \<char|none\>
      Local character used to detach from the session without disturbing the remote command. The default is **^]** (0x1d). Use `none` to disable it.

  -r, --reconnect \<n\>
      Maximum number of reconnect attempts after a connection failure. Use -1 for unlimited attempts. The default is 5.

  --no-reconnect
      Never reconnect. Equivalent to `--reconnect 0`.

  --reconnect-on-eof
      Also reconnect when the server ends the session, which happens when the remote command exits. This matches the behaviour of the browser client, which shows "Press Enter to Reconnect" instead of closing the page.

  --reconnect-delay \<ms\>
      Initial delay between reconnect attempts in milliseconds (default: 1000). The delay doubles after every failed attempt.

  --reconnect-max-delay \<ms\>
      Upper bound for the reconnect delay in milliseconds (default: 30000).

  --connect-timeout \<seconds\>
      Timeout for the TCP connect, the TLS handshake and the websocket upgrade (default: 15, 0 disables the timeout).

  -P, --ping-interval \<seconds\>
      Interval of the client side websocket keepalive pings (default: 5, 0 disables them). The ttyd server closes idle connections after about 10 seconds, so pongs are always answered even when pings are disabled.

  --flow-limit \<bytes\>
      Number of output bytes written between two flow control checks (default: 100000), mirroring the web client.

  --flow-high-water \<bytes\>
      Output backlog size that makes the client send a **PAUSE** message to the server (default: 1048576).

  --flow-low-water \<bytes\>
      Output backlog size that makes the client send a **RESUME** message again (default: 262144).

  --cols \<columns\>, --rows \<rows\>
      Initial terminal size. By default the size of the local terminal is used.

  --no-resize
      Never send **RESIZE_TERMINAL** messages, even when the local terminal is resized.

  --title \<title\>
      Use a fixed local window title instead of the one sent by the server.

  --no-title
      Never change the local window title.

  -S, --ssl
      Use TLS (wss) for urls that do not contain a scheme.

  -k, --insecure
      Do not verify the server certificate.

  -C, --ssl-cert \<path\>
      Client certificate (PEM) used when the server requests one.

  -K, --ssl-key \<path\>
      Private key (PEM) belonging to the client certificate.

  -A, --ssl-ca \<path\>
      CA bundle used to verify the server certificate, instead of the platform trust store. On Windows the platform store is the Windows certificate store (current user and local machine `ROOT`); elsewhere it is the OpenSSL default CA file and directory, with a fallback to the conventional system bundle locations (for example */etc/ssl/certs/ca-certificates.crt* and */etc/pki/tls/certs/ca-bundle.crt*). The **SSL_CERT_FILE** and **SSL_CERT_DIR** environment variables override the default paths. Static binaries do not embed a CA bundle and use the trust store of the machine they run on.

  -x, --proxy \<url\>
      HTTP proxy used for the connection (format: `http://[user:password@]host[:port]`). By default the **https_proxy**, **http_proxy** or **all_proxy** environment variables are used.

  --no-proxy
      Never use a proxy.

  -q, --quiet
      Only print errors.

  -d, --debug \<level\>
      Log level bitmask, using the same values as `ttyd --debug`: 1 error, 2 warning, 4 notice, 8 info, 16 debug, 1024 timestamps. The default is 6 (errors, warnings and notices). Use 31 to also see debug and info messages.

  -v, --version
      Print the version and exit.

  -h, --help
      Print the help text and exit.

# CLIENT OPTIONS
  The server can send client preferences with the **SET_PREFERENCES** message (the --client-option option of ttyd, or the query string of a browser url). ttyd-client understands the following keys:

  **titleFixed**
      Use this string as a fixed window title.

  **disableReconnect**, **closeOnDisconnect**
      Disable the automatic reconnection.

  **disableStdin**
      Stop forwarding keyboard input.

  **disableTitle**
      Do not change the window title.

  All other keys are ignored but shown at debug level. Local --client-option values take precedence over the values sent by the server.

# KEY BINDINGS
  **Ctrl-]** (or the character given with --escape-char)
      Detach from the session. The websocket is closed cleanly; whether the remote command keeps running depends on the server options (--once/--exit-no-conn terminate it, a plain server keeps it alive until a client reconnects).

  Every other key sequence is forwarded to the remote terminal. While the local terminal is in raw mode Ctrl-C is passed to the remote shell, so use Ctrl-] or a signal to leave the client.

# ENVIRONMENT
  **https_proxy**, **HTTPS_PROXY**, **http_proxy**, **HTTP_PROXY**, **all_proxy**, **ALL_PROXY**
      Proxy used for the connection when --proxy is not given. Ignored with --no-proxy.

  **COLUMNS**, **LINES**
      Fallback terminal size when it cannot be queried.

# EXIT STATUS
  **0**
      The session ended normally, either because the server closed it with the websocket code 1000 (the remote command exited successfully), because the remote terminal was closed, or because the user pressed Ctrl-].

  **1**
      Connection, authentication or protocol error, an abnormal close code, or no remaining reconnect attempts.

  **2**
      Invalid command line usage.

# EXAMPLES

  Start a server and attach to it:

```bash
ttyd -W bash
ttyd-client localhost:7681
```

  Connect to a password protected server mounted behind a reverse proxy:

```bash
ttyd-client -c user:pass https://example.com/terminal/
```

  Watch a session without being able to type into it:

```bash
ttyd-client --readonly wss://example.com/terminal/ws
```

  Pass extra command line arguments to the remote command:

```bash
ttyd -W -a bash                  # server side
ttyd-client -a top localhost:7681 # runs: bash top
```

  Trust a private certificate authority:

```bash
ttyd-client -A ./ca.pem wss://localhost:7681/ws
```

# PROTOCOL
  Every websocket message is a binary frame whose first byte is the command character, followed by the payload.

```
client -> server          server -> client
0 INPUT                   0 OUTPUT
1 RESIZE_TERMINAL         1 SET_WINDOW_TITLE
2 PAUSE                   2 SET_PREFERENCES
3 RESUME
{ JSON_DATA (AuthToken, columns, rows)
```

  The initial **JSON_DATA** message starts the remote process and must be the first message sent by the client. The terminal size is part of it as well as of **RESIZE_TERMINAL**, in the form `{"columns":W,"rows":H}`.

# SEE ALSO
  **ttyd**(1), **ssh**(1), **lrzsz**(1)

  Full documentation and the source code: https://github.com/tsl0922/ttyd
