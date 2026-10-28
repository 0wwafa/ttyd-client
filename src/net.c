#include "net.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "utils.h"

#ifndef _WIN32
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#ifdef MSG_NOSIGNAL
#define TC_SEND_FLAGS MSG_NOSIGNAL
#else
#define TC_SEND_FLAGS 0
#endif

#ifdef _WIN32
#define TC_ETIMEDOUT WSAETIMEDOUT
typedef int tc_socklen_t;
#else
#define TC_ETIMEDOUT ETIMEDOUT
typedef socklen_t tc_socklen_t;
#endif

bool net_init(void) {
#ifdef _WIN32
  WSADATA wsa;
  return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
  return true;
#endif
}

void net_cleanup(void) {
#ifdef _WIN32
  WSACleanup();
#endif
}

int net_last_error(void) {
#ifdef _WIN32
  return WSAGetLastError();
#else
  return errno;
#endif
}

const char *net_error_str(int code, char *buf, size_t len) {
#ifdef _WIN32
  DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
  DWORD n = FormatMessageA(flags, NULL, (DWORD)code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buf, (DWORD)len, NULL);
  if (n == 0) snprintf(buf, len, "winsock error %d", code);
  /* strip trailing CR/LF */
  for (size_t i = 0; i < len && buf[i]; i++) {
    if (buf[i] == '\r' || buf[i] == '\n') {
      buf[i] = '\0';
      break;
    }
  }
  return buf;
#else
  snprintf(buf, len, "%s", strerror(code));
  return buf;
#endif
}

bool net_set_blocking(socket_t sock, bool blocking) {
#ifdef _WIN32
  u_long mode = blocking ? 0 : 1;
  return ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
  int flags = fcntl(sock, F_GETFL, 0);
  if (flags < 0) return false;
  if (blocking) {
    flags &= ~O_NONBLOCK;
  } else {
    flags |= O_NONBLOCK;
  }
  return fcntl(sock, F_SETFL, flags) == 0;
#endif
}

bool net_set_io_timeout(socket_t sock, int recv_ms, int send_ms) {
#ifdef _WIN32
  DWORD r = (DWORD)recv_ms;
  DWORD s = (DWORD)send_ms;
  bool ok = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&r, sizeof(r)) == 0;
  ok = setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&s, sizeof(s)) == 0 && ok;
  return ok;
#else
  struct timeval tv;
  tv.tv_sec = recv_ms / 1000;
  tv.tv_usec = (recv_ms % 1000) * 1000;
  bool ok = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
  tv.tv_sec = send_ms / 1000;
  tv.tv_usec = (send_ms % 1000) * 1000;
  ok = setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0 && ok;
  return ok;
#endif
}

bool net_wait_readable(socket_t sock, int timeout_ms) {
  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(sock, &rfds);

  struct timeval tv;
  struct timeval *tvp = NULL;
  if (timeout_ms >= 0) {
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    tvp = &tv;
  }
#ifdef _WIN32
  int n = select(0, &rfds, NULL, NULL, tvp);
#else
  int n = select(sock + 1, &rfds, NULL, NULL, tvp);
#endif
  return n > 0;
}

bool net_wait_writable(socket_t sock, int timeout_ms) {
  fd_set wfds;
  FD_ZERO(&wfds);
  FD_SET(sock, &wfds);

  struct timeval tv;
  struct timeval *tvp = NULL;
  if (timeout_ms >= 0) {
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    tvp = &tv;
  }
#ifdef _WIN32
  int n = select(0, NULL, &wfds, NULL, tvp);
#else
  int n = select(sock + 1, NULL, &wfds, NULL, tvp);
#endif
  return n > 0;
}

socket_t net_connect(const char *host, int port, int timeout_ms, char *err, size_t err_len) {
  char port_str[16];
  snprintf(port_str, sizeof(port_str), "%d", port);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  struct addrinfo *result = NULL;
  int rc = getaddrinfo(host, port_str, &hints, &result);
  if (rc != 0) {
#ifdef _WIN32
    snprintf(err, err_len, "cannot resolve %s: %d", host, rc);
#else
    snprintf(err, err_len, "cannot resolve %s: %s", host, gai_strerror(rc));
#endif
    return TC_INVALID_SOCKET;
  }

  socket_t sock = TC_INVALID_SOCKET;
  int last_err = 0;

  for (struct addrinfo *ai = result; ai != NULL; ai = ai->ai_next) {
    sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (sock == TC_INVALID_SOCKET) {
      last_err = net_last_error();
      continue;
    }

    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
#ifdef SO_NOSIGPIPE
    /* macOS/BSD: prevent SIGPIPE when the peer closes the connection */
    setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&one, sizeof(one));
#endif

    if (timeout_ms > 0) {
      net_set_blocking(sock, false);
    }

    rc = connect(sock, ai->ai_addr, (int)ai->ai_addrlen);
    if (rc != 0) {
      int code = net_last_error();
      bool in_progress = false;
#ifdef _WIN32
      in_progress = (code == WSAEWOULDBLOCK || code == WSAEINPROGRESS);
#else
      in_progress = (code == EINPROGRESS);
#endif
      if (!in_progress) {
        last_err = code;
        net_close(sock);
        sock = TC_INVALID_SOCKET;
        continue;
      }
      if (!net_wait_writable(sock, timeout_ms)) {
        last_err = TC_ETIMEDOUT;
        net_close(sock);
        sock = TC_INVALID_SOCKET;
        continue;
      }
      int so_err = 0;
      tc_socklen_t so_len = sizeof(so_err);
      if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&so_err, &so_len) != 0 || so_err != 0) {
        last_err = so_err ? so_err : net_last_error();
        net_close(sock);
        sock = TC_INVALID_SOCKET;
        continue;
      }
    }

    if (timeout_ms > 0) net_set_blocking(sock, true);
    freeaddrinfo(result);
    return sock;
  }

  freeaddrinfo(result);
  char msg[128];
  net_error_str(last_err, msg, sizeof(msg));
  snprintf(err, err_len, "cannot connect to %s:%d: %s", host, port, msg);
  return TC_INVALID_SOCKET;
}

int net_read(socket_t sock, void *buf, size_t len) {
  if (len == 0) return 0;
#ifdef _WIN32
  int n = recv(sock, (char *)buf, (int)(len > 0x7fffffff ? 0x7fffffff : len), 0);
  if (n == SOCKET_ERROR) {
    int code = WSAGetLastError();
    if (code == WSAEWOULDBLOCK) return TC_IO_AGAIN;
    return TC_IO_ERROR;
  }
  return n == 0 ? TC_IO_EOF : n;
#else
  ssize_t n;
  do {
    n = recv(sock, buf, len, 0);
  } while (n < 0 && errno == EINTR);
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return TC_IO_AGAIN;
    return TC_IO_ERROR;
  }
  return (int)(n == 0 ? TC_IO_EOF : n);
#endif
}

int net_write(socket_t sock, const void *buf, size_t len) {
  if (len == 0) return 0;
#ifdef _WIN32
  int n = send(sock, (const char *)buf, (int)(len > 0x7fffffff ? 0x7fffffff : len), 0);
  if (n == SOCKET_ERROR) return TC_IO_ERROR;
  return n;
#else
  ssize_t n;
  do {
    n = send(sock, buf, len, TC_SEND_FLAGS);
  } while (n < 0 && errno == EINTR);
  if (n < 0) return TC_IO_ERROR;
  return (int)n;
#endif
}

void net_shutdown(socket_t sock) {
  if (sock == TC_INVALID_SOCKET) return;
#ifdef _WIN32
  shutdown(sock, SD_BOTH);
#else
  shutdown(sock, SHUT_RDWR);
#endif
}

void net_close(socket_t sock) {
  if (sock == TC_INVALID_SOCKET) return;
#ifdef _WIN32
  closesocket(sock);
#else
  close(sock);
#endif
}
