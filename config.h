#ifndef MEMBER4_CONFIG_H
#define MEMBER4_CONFIG_H

#include <stdint.h>

/* These formats match the course handout. The existing peer.c uses its own
 * equivalent client-config parser and is intentionally left unchanged.
 */
typedef struct {
    uint16_t tracker_port;
    char tracker_ip[46];  /* numeric IPv4 or IPv6, NUL-terminated */
    unsigned long refresh_seconds;
} client_config_t;

typedef struct {
    uint16_t listen_port;
    char shared_dir[4096];
} server_config_t;

/* serverThreadConfig.cfg and sconfig have the same two-line layout:
 *   line 1: TCP listen port
 *   line 2: shared folder path
 * Returns 0 on success or -1 with errno on failure.
 */
int config_read_server(const char *path, server_config_t *out);
int config_read_tracker(const char *path, server_config_t *out);

/* clientThreadConfig.cfg: tracker port, numeric tracker IP, refresh seconds. */
int config_read_client(const char *path, client_config_t *out);

#endif
