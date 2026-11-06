/* Small test-only CLI to exercise both config parsers without changing peer.c. */
#include "config.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    if (!strcmp(argv[1], "client")) {
        client_config_t c;
        if (config_read_client(argv[2], &c) < 0) return 1;
        printf("%u %s %lu\n", (unsigned)c.tracker_port, c.tracker_ip,
               c.refresh_seconds);
        return 0;
    }
    if (!strcmp(argv[1], "server") || !strcmp(argv[1], "tracker")) {
        server_config_t c;
        int rc = !strcmp(argv[1], "server") ?
            config_read_server(argv[2], &c) : config_read_tracker(argv[2], &c);
        if (rc < 0) return 1;
        printf("%u %s\n", (unsigned)c.listen_port, c.shared_dir);
        return 0;
    }
    return 2;
}
