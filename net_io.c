#define _POSIX_C_SOURCE 200809L
#include "net_io.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

int net_set_socket_timeouts(int fd, int timeout_ms) {
    if (fd < 0 || timeout_ms <= 0) {
        errno = EINVAL;
        return -1;
    }
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) < 0) return -1;
    return setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

static int64_t monotonic_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return -1;
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int net_connect_tcp(const char *numeric_host, uint16_t port, int timeout_ms) {
    if (!numeric_host || !*numeric_host || port == 0 || timeout_ms <= 0) {
        errno = EINVAL;
        return -1;
    }
    char service[6];
    snprintf(service, sizeof service, "%u", (unsigned)port);
    struct addrinfo hint = {0}, *addresses = NULL;
    hint.ai_family = AF_UNSPEC;
    hint.ai_socktype = SOCK_STREAM;
    hint.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    int rc = getaddrinfo(numeric_host, service, &hint, &addresses);
    if (rc != 0) {
        errno = EINVAL;
        return -1;
    }
    int result = -1, last_error = ECONNREFUSED;
    int64_t started_at = monotonic_ms();
    if (started_at < 0) { freeaddrinfo(addresses); return -1; }
    int64_t deadline = started_at + timeout_ms;
    for (struct addrinfo *addr = addresses; addr; addr = addr->ai_next) {
        int fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (fd < 0) { last_error = errno; continue; }
        int flags = fcntl(fd, F_GETFL);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            last_error = errno; close(fd); continue;
        }
        if (connect(fd, addr->ai_addr, addr->ai_addrlen) < 0) {
            if (errno != EINPROGRESS) { last_error = errno; close(fd); continue; }
            int connected = 0;
            while (!connected) {
                int64_t left = deadline - monotonic_ms();
                if (left <= 0) { last_error = ETIMEDOUT; break; }
                struct pollfd pfd = {fd, POLLOUT, 0};
                rc = poll(&pfd, 1, left > INT_MAX ? INT_MAX : (int)left);
                if (rc < 0 && errno == EINTR) continue;
                if (rc <= 0) { last_error = rc == 0 ? ETIMEDOUT : errno; break; }
                int pending_error = 0;
                socklen_t n = sizeof pending_error;
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &pending_error, &n) < 0) {
                    last_error = errno; break;
                }
                if (pending_error != 0) { last_error = pending_error; break; }
                connected = 1;
            }
            if (!connected) { close(fd); continue; }
        }
        if (fcntl(fd, F_SETFL, flags) < 0 || net_set_socket_timeouts(fd, timeout_ms) < 0) {
            last_error = errno; close(fd); continue;
        }
        result = fd;
        break;
    }
    freeaddrinfo(addresses);
    if (result < 0) errno = last_error;
    return result;
}

int net_send_all(int fd, const void *data, size_t len) {
    if (fd < 0 || (!data && len)) { errno = EINVAL; return -1; }
    const unsigned char *p = data;
    size_t offset = 0;
    while (offset < len) {
        size_t count = len - offset;
        if (count > (size_t)SSIZE_MAX) count = (size_t)SSIZE_MAX;
#ifdef MSG_NOSIGNAL
        ssize_t n = send(fd, p + offset, count, MSG_NOSIGNAL);
#else
        ssize_t n = send(fd, p + offset, count, 0);
#endif
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        if (n == 0) { errno = EPIPE; return -1; }
        offset += (size_t)n;
    }
    return 0;
}

int net_recv_exact(int fd, void *buffer, size_t len) {
    if (fd < 0 || (!buffer && len)) { errno = EINVAL; return -1; }
    unsigned char *p = buffer;
    size_t offset = 0;
    while (offset < len) {
        size_t count = len - offset;
        if (count > (size_t)SSIZE_MAX) count = (size_t)SSIZE_MAX;
        ssize_t n = recv(fd, p + offset, count, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        if (n == 0) { errno = ECONNRESET; return -1; }
        offset += (size_t)n;
    }
    return 0;
}

ssize_t net_recv_line(int fd, char *buffer, size_t cap) {
    if (fd < 0 || !buffer || cap < 2) { errno = EINVAL; return -1; }
    size_t n = 0;
    while (n + 1 < cap) {
        char ch;
        ssize_t got = recv(fd, &ch, 1, 0);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) return -1;
        if (got == 0) { errno = ECONNRESET; return -1; }
        if (ch == '\0') { errno = EPROTO; return -1; }
        buffer[n++] = ch;
        if (ch == '\n') { buffer[n] = '\0'; return (ssize_t)n; }
    }
    buffer[n] = '\0';
    errno = EMSGSIZE;
    return -1;
}
