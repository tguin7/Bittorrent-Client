/* Direct transport tests: nonblocking connect, line framing, exact binary IO. */
#define _POSIX_C_SOURCE 200809L
#include "net_io.h"
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define REQUIRE(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL line %d: %s (errno=%d)\n", __LINE__, #cond, errno); \
    return 1; \
} } while (0)

static void *write_fragments(void *arg) {
    int fd = *(int *)arg;
    (void)net_send_all(fd, "<DATA 5>\n", 9);
    (void)net_send_all(fd, "a\0", 2);
    (void)net_send_all(fd, "bcd", 3);
    return NULL;
}

int main(void) {
    int pair[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    REQUIRE(net_set_socket_timeouts(pair[0], 500) == 0);
    REQUIRE(net_set_socket_timeouts(pair[1], 500) == 0);
    pthread_t writer;
    REQUIRE(pthread_create(&writer, NULL, write_fragments, &pair[1]) == 0);
    char line[30];
    REQUIRE(net_recv_line(pair[0], line, sizeof line) == 9);
    REQUIRE(strcmp(line, "<DATA 5>\n") == 0);
    unsigned char raw[5];
    REQUIRE(net_recv_exact(pair[0], raw, sizeof raw) == 0);
    REQUIRE(memcmp(raw, "a\0bcd", 5) == 0);
    pthread_join(writer, NULL);
    close(pair[0]); close(pair[1]);

    /* Local ephemeral TCP listener for exercising the connection utility. */
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listener >= 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    REQUIRE(bind(listener, (struct sockaddr *)&addr, sizeof addr) == 0);
    REQUIRE(listen(listener, 1) == 0);
    socklen_t addrlen = sizeof addr;
    REQUIRE(getsockname(listener, (struct sockaddr *)&addr, &addrlen) == 0);
    int client = net_connect_tcp("127.0.0.1", ntohs(addr.sin_port), 500);
    REQUIRE(client >= 0);
    int accepted = accept(listener, NULL, NULL);
    REQUIRE(accepted >= 0);
    REQUIRE(net_send_all(client, "<PING a>\n", 9) == 0);
    REQUIRE(net_recv_line(accepted, line, sizeof line) == 9);
    REQUIRE(strcmp(line, "<PING a>\n") == 0);
    REQUIRE(net_set_socket_timeouts(client, 100) == 0);
    REQUIRE(recv(client, line, 1, 0) == -1);
    REQUIRE(errno == EAGAIN || errno == EWOULDBLOCK);
    close(client); close(accepted); close(listener);
    REQUIRE(net_connect_tcp("not-an-ip", 3456, 100) == -1);
    REQUIRE(errno == EINVAL);
    puts("Member 4 network I/O tests passed (framing, binary, TCP connect, timeout).");
    return 0;
}
