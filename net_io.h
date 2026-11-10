#ifndef MEMBER4_NET_IO_H
#define MEMBER4_NET_IO_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* All functions return -1 on failure and set errno (except where noted).
 * Callers should set a finite socket timeout before using blocking I/O.
 * This module is transport-only: it does not interpret protocol commands.
 */

/* Sets both receive and send timeouts. timeout_ms must be positive. */
int net_set_socket_timeouts(int fd, int timeout_ms);

/* Numeric IPv4/IPv6 address, TCP; nonblocking connect with a timeout.
 * Returns a connected, blocking socket with I/O timeouts, or -1. Caller owns fd.
 * sockaddr ports are encoded using htons() by getaddrinfo().
 */
int net_connect_tcp(const char *numeric_host, uint16_t port, int timeout_ms);

/* Transfer exactly len raw bytes; 0 success, -1 error. No implicit delimiter.
 * Uses MSG_NOSIGNAL on Linux to avoid SIGPIPE from a disconnected peer.
 */
int net_send_all(int fd, const void *data, size_t len);
int net_recv_exact(int fd, void *buffer, size_t len);

/* Read one newline-terminated text line into buffer including '\n', append '\0'.
 * Returns number of bytes (including '\n') on success; -1 on EOF, NUL,
 * truncation, timeout, or socket error. cap must be at least 2.
 * Reads one byte at a time so a following binary payload is never consumed.
 */
ssize_t net_recv_line(int fd, char *buffer, size_t cap);

#endif
