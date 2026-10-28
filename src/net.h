#ifndef TTYD_CLIENT_NET_H
#define TTYD_CLIENT_NET_H

#include <stdbool.h>
#include <stddef.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
#define TC_INVALID_SOCKET INVALID_SOCKET
#else
typedef int socket_t;
#define TC_INVALID_SOCKET (-1)
#endif

/* Return codes shared by net_read()/net_write() */
#define TC_IO_ERROR (-1)
#define TC_IO_EOF 0
#define TC_IO_AGAIN (-2)

/* Initialize the platform network stack (WSAStartup on Windows) */
bool net_init(void);

/* Clean up the network stack */
void net_cleanup(void);

/* Resolve and connect to host:port, honouring `timeout_ms` (0 = no timeout).
 * Returns TC_INVALID_SOCKET and fills `err` on failure. */
socket_t net_connect(const char *host, int port, int timeout_ms, char *err, size_t err_len);

/* Read up to `len` bytes, returns the number of bytes read, TC_IO_EOF or TC_IO_ERROR */
int net_read(socket_t sock, void *buf, size_t len);

/* Write up to `len` bytes, returns the number of bytes written or TC_IO_ERROR */
int net_write(socket_t sock, const void *buf, size_t len);

/* Close a socket */
void net_close(socket_t sock);

/* Wait until the socket is readable/writable, returns false on error/timeout */
bool net_wait_readable(socket_t sock, int timeout_ms);
bool net_wait_writable(socket_t sock, int timeout_ms);

/* Switch between blocking and non blocking mode */
bool net_set_blocking(socket_t sock, bool blocking);

/* Set receive/send timeouts in milliseconds (0 disables the timeout) */
bool net_set_io_timeout(socket_t sock, int recv_ms, int send_ms);

/* Last socket error code for the calling thread */
int net_last_error(void);

/* Human readable socket error string */
const char *net_error_str(int code, char *buf, size_t len);

/* Shut down both directions of the socket without closing it */
void net_shutdown(socket_t sock);

#endif  // TTYD_CLIENT_NET_H
