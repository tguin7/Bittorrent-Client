#define _POSIX_C_SOURCE 200809L
#include "config.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Strict fixed-line format: no hidden values, blank extra lines are allowed.
 * Using getline prevents silently accepting a truncated port or directory.
 */
static int read_lines(const char *path, char lines[][4096], size_t wanted) {
    if (!path || !lines || !wanted) { errno = EINVAL; return -1; }
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char *buf = NULL;
    size_t capacity = 0;
    ssize_t n;
    size_t i = 0;
    int ok = 1, err = EINVAL;
    while ((n = getline(&buf, &capacity, f)) != -1) {
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
        if (memchr(buf, '\0', (size_t)n) != NULL) { ok = 0; break; }
        if (i < wanted) {
            if (n == 0 || (size_t)n >= sizeof lines[0]) { ok = 0; break; }
            memcpy(lines[i++], buf, (size_t)n + 1);
        } else {
            for (ssize_t j = 0; j < n; ++j) {
                if (!isspace((unsigned char)buf[j])) { ok = 0; break; }
            }
            if (!ok) break;
        }
    }
    if (ferror(f)) { ok = 0; err = EIO; }
    if (i != wanted) ok = 0;
    free(buf);
    if (fclose(f) != 0) { ok = 0; err = EIO; }
    if (!ok) { errno = err; return -1; }
    return 0;
}

static int positive_number(const char *s, unsigned long *out) {
    if (!s || !*s) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
        if (!isdigit(*p)) return 0;
    errno = 0;
    char *end;
    unsigned long value = strtoul(s, &end, 10);
    if (errno || *end || !value) return 0;
    *out = value;
    return 1;
}

int config_read_server(const char *path, server_config_t *out) {
    if (!out) { errno = EINVAL; return -1; }
    char lines[2][4096] = {{0}};
    if (read_lines(path, lines, 2) != 0) return -1;
    unsigned long port;
    if (!positive_number(lines[0], &port) || port > UINT16_MAX ||
        strpbrk(lines[1], "\t\r\n") || lines[1][0] == ' ' ||
        lines[1][strlen(lines[1])-1] == ' ') {
        errno = EINVAL;
        return -1;
    }
    server_config_t tmp = {0};
    tmp.listen_port = (uint16_t)port;
    memcpy(tmp.shared_dir, lines[1], strlen(lines[1]) + 1);
    *out = tmp;
    return 0;
}

int config_read_tracker(const char *path, server_config_t *out) {
    return config_read_server(path, out);
}

int config_read_client(const char *path, client_config_t *out) {
    if (!out) { errno = EINVAL; return -1; }
    char lines[3][4096] = {{0}};
    if (read_lines(path, lines, 3) != 0) return -1;
    unsigned long port, refresh;
    unsigned char addr[16];
    if (!positive_number(lines[0], &port) || port > UINT16_MAX ||
        (inet_pton(AF_INET, lines[1], addr) != 1 &&
         inet_pton(AF_INET6, lines[1], addr) != 1) ||
        strlen(lines[1]) >= sizeof out->tracker_ip ||
        !positive_number(lines[2], &refresh)) {
        errno = EINVAL;
        return -1;
    }
    client_config_t tmp = {0};
    tmp.tracker_port = (uint16_t)port;
    strcpy(tmp.tracker_ip, lines[1]);
    tmp.refresh_seconds = refresh;
    *out = tmp;
    return 0;
}
