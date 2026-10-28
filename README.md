# ttyd-client - a terminal client for ttyd

![backend](https://github.com/0wwafa/ttyd-client/workflows/backend/badge.svg)
![release](https://github.com/0wwafa/ttyd-client/workflows/release/badge.svg)
![GitHub](https://img.shields.io/github/license/0wwafa/ttyd-client)

`ttyd-client` is a companion command line client for [ttyd](https://github.com/tsl0922/ttyd).
It attaches your terminal to a terminal shared by a ttyd server, so a shared
session can be used from a shell exactly like `ssh`, without a browser.

```
$ ttyd -W bash &
$ ttyd-client localhost:7681
user@host:~$ █
```

It is a pure C program that speaks the same websocket protocol as the ttyd web
frontend, but needs no JavaScript, no browser and no other run time
dependencies.

## Features

- **Same protocol as the web client**: `/token` authentication, the `JSON_DATA`
  bootstrap message, `INPUT`, `OUTPUT`, `RESIZE_TERMINAL`, `PAUSE` and `RESUME`
- **Full terminal integration**: raw mode, resize forwarding, window title
  updates, terminal size reporting
- **Basic authentication** (`-c user:pass`), reverse proxy auth headers
  (`-H name=value`) and client arguments passed in the url (`-a key=value`)
- **TLS** (`wss://`) with certificate verification, client certificates and
  configurable CA bundles, plus **HTTP proxy** tunnelling (`CONNECT`)
- **Flow control**: pauses the remote terminal instead of buffering without
  limit when the local terminal (or a pipe) is slower than the output
- **Automatic reconnection** with exponential backoff
- **Client preferences**: applies `SET_PREFERENCES` sent by the server and lets
  you override them with `-t key=value`
- **Zero dependencies**: TLS is optional (OpenSSL), everything else
  (websocket, HTTP, JSON, SHA-1, base64, URL parsing) is built in
- **Cross platform**: Linux (glibc and musl), macOS, FreeBSD/OpenBSD,
  OpenWrt, Windows (MSVC and MinGW). Static binaries for 13 architectures,
  built exactly like the ttyd release binaries

## Installation

### Precompiled static binaries

Download the archive for your platform from the
[releases](https://github.com/0wwafa/ttyd-client/releases) page. The Linux
binaries are statically linked against musl and have no run time dependencies.

### Build from source

Requirements: a C99 compiler and CMake >= 3.12. OpenSSL is optional and only
needed for `wss://` / `https://` urls.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
sudo cmake --install build        # installs ttyd-client and the man page
```

Without CMake:

```bash
make cc                           # uses cc, builds ./ttyd-client
```

Fully static (musl) build:

```bash
sudo apt install musl-tools
make static                       # no TLS, static, stripped
make static TLS=1                 # same but with OpenSSL (needs a static libssl)
```

Cross compilation uses the same musl toolchains as ttyd:

```bash
env BUILD_TARGET=aarch64 ./scripts/cross-build.sh
env BUILD_TARGET=win32 ./scripts/cross-build.sh
```

The script builds OpenSSL for the target (unless `BUILD_TLS=0`) and produces
`build/ttyd-client.<target>`.

### Windows

- Binary releases are static executables built with MSVC (`x64-windows-static`)
  and with MinGW (`x86_64-w64-mingw32`).
- To compile on Windows, use the same vcpkg triplet as ttyd:

```powershell
vcpkg install --triplet x64-windows-static openssl getopt-win32
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release
```

## Usage

```
ttyd-client [options] <url>
```

The url can be a plain `host[:port]` (default port 7681, `ws://`), a
`ws://`/`wss://` url, or an `http(s)://` url pointing at the base path of the
server. `/ws` and `/token` are derived automatically:

```bash
ttyd-client localhost:7681
ttyd-client ws://localhost:7681/ws
ttyd-client https://example.com/terminal/          # server behind a reverse proxy
ttyd-client -c user:pass wss://example.com/terminal/
```

Common options (see [the man page](man/ttyd-client.1) for all of them):

| Option | Description |
| --- | --- |
| `-c, --credential user:pass` | Basic authentication |
| `-H, --auth-header name=value` | Extra HTTP header (auth proxy), repeatable |
| `-a, --url-arg key=value` | Forward an argument to the remote command (`--url-arg` server option) |
| `-t, --client-option key=value` | Override a server client preference, repeatable |
| `-R, --readonly` | Do not send input |
| `-r, --reconnect N` | Reconnect attempts (default 5, -1 unlimited) |
| `--reconnect-on-eof` | Also reconnect after the remote command exits |
| `-P, --ping-interval S` | Keepalive interval in seconds (default 5, 0 disables) |
| `-S, --ssl` / `-k, --insecure` | Force TLS / skip verification |
| `-A/-C/-K` | CA bundle / client certificate / client key |
| `-x, --proxy url` / `--no-proxy` | HTTP proxy (default: environment) |
| `-d, --debug level` | Same levels as `ttyd --debug` |

Press **Ctrl-]** to detach from the session (configurable with
`--escape-char`). Everything else, including Ctrl-C, Ctrl-Z and the arrow
keys, is forwarded to the remote terminal.

### Pipes and automation

`ttyd-client` works without a tty on stdin/stdout, which makes it usable in
scripts:

```bash
printf 'uname -a\r\nexit\r\n' | ttyd-client --readonly localhost:7681
```

When stdin reaches end of file the client sends `^D` by default
(`--eof-action eot|quit|ignore`), and it exits with `0` when the server ends
the session with the websocket close code 1000.

## Protocol

The client speaks the ttyd terminal protocol; each websocket binary message
starts with a command byte:

| Direction | Byte | Message |
| --- | --- | --- |
| client → server | `0` | `INPUT`, keyboard data |
| client → server | `1` | `RESIZE_TERMINAL`, `{"columns":W,"rows":H}` |
| client → server | `2` / `3` | `PAUSE` / `RESUME` |
| client → server | `{` | `JSON_DATA`, `{"AuthToken":"...","columns":W,"rows":H}` |
| server → client | `0` | `OUTPUT`, terminal data |
| server → client | `1` | `SET_WINDOW_TITLE` |
| server → client | `2` | `SET_PREFERENCES`, JSON client options |

The `JSON_DATA` message starts the remote process and must be sent right after
the websocket handshake, once the `/token` endpoint (if authentication is
configured) has been queried for the token. Websocket pings from the server
are answered immediately, because ttyd drops clients that stay silent for
about 10 seconds.

### Differences from the browser client

The browser client keeps the page open when the remote command exits and
offers a reconnect; `ttyd-client` exits with status `0` instead, because a
terminal client should return when the remote session ends. Use
`--reconnect-on-eof` for the browser behaviour.

Features that only make sense in a browser (zmodem/trzsz drag & drop, sixel
rendering, WebGL renderer, unicode version switching) are not implemented:
the terminal emulation itself is done by the terminal you run the client in.

## Development

```bash
make test                  # unit tests + integration tests against a real ttyd
make format                # clang-format with the bundled .clang-format
```

Layout, mirroring the ttyd repository:

```
src/     C sources (client, protocol, websocket, http, io, tls, term, ...)
test/    unit tests (C) and the integration test suite (Python, drives a pty)
man/     man page sources
scripts/ cross build script for all supported architectures
.github/ CI workflows (backend matrix, release, docker)
```

The CI matrix builds and tests the client on Linux (glibc and musl), macOS and
Windows, cross compiles it for all architectures supported by the ttyd
releases (`i686`, `x86_64`, `arm`, `armhf`, `aarch64`, `mips`, `mipsel`,
`mips64`, `mips64el`, `ppc64`, `ppc64le`, `s390x`, `win32`), and publishes
tagged releases with sha256 sums.

## License

MIT, see [LICENSE](LICENSE). `ttyd-client` is an independent companion project
for [ttyd](https://github.com/tsl0922/ttyd) by Shuanglei Tao, which is also
MIT licensed.
