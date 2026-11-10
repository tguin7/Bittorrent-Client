// Start the file server in the background and pass tracker commands to peer.c.
#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "net_io.h"
#include "node_download.h"
#include "peer_server.h"
#include "thread_server.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested = 0;
static volatile sig_atomic_t server_failed = 0;

static void stop_handler(int signum) {
    (void)signum;
    stop_requested = 1;
}
typedef struct {
    thread_server_options_t opts;
    server_config_t server_cfg;
} server_t;

static void *server_main(void *arg) {
    server_t *s = arg;
    if (thread_server_run(&s->opts, peer_serve_connection,
                          s->server_cfg.shared_dir, &stop_requested) < 0) {
        perror("[node] peer server");
        server_failed = 1;
    }
    return NULL;
}

// Run the original peer client as a separate process, without modifying peer.c.
static int run_client(char *const command[]) {
    char *args[16] = {"./peer"};
    unsigned i = 1;
    for (unsigned j = 0; command[j]; j++) {
        if (i >= 15) {
            fprintf(stderr, "[node] Too many command fields\n");
            return -1;
        }
        args[i++] = command[j];
    }
    args[i] = NULL;
    fflush(stdout);

    // Wait for the client command to finish before reading another command.
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        execv("./peer", args);
        perror("[node] exec peer");
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        perror("waitpid");
        return -1;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}
static int execute_command(char *const args[], server_t *server) {
    if (!args[0]) return 0;

    if (!strcmp(args[0], "GET") && args[1] && !args[2]) {
        // GET saves the verified .track file first, then downloads the actual file.
        if (run_client(args) != 0) return -1;
        char cache[4096];
        int n = snprintf(cache, sizeof cache, "cache/%s", args[1]);
        if (n < 0 || (size_t)n >= sizeof cache) return -1;
        return node_download_from_cache(cache, server->server_cfg.shared_dir);
    }

    return run_client(args);
}

static int command_tokens(char *line, char **fields, unsigned limit) {
    unsigned n = 0;
    char *save = NULL;
    for (char *p = strtok_r(line, " \t\r\n", &save); p;
         p = strtok_r(NULL, " \t\r\n", &save)) {
        if (n >= limit - 1) return -1;
        fields[n++] = p;
    }
    fields[n] = NULL;
    return (int)n;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, stop_handler);
    signal(SIGINT, stop_handler);
    server_t server = {0};
    client_config_t client;
    if (config_read_server("serverThreadConfig.cfg", &server.server_cfg) != 0 ||
        config_read_client("clientThreadConfig.cfg", &client) != 0) {
        perror("[node] configuration");
        return 1;
    }
    if (mkdir(server.server_cfg.shared_dir, 0700) < 0 && errno != EEXIST) {
        perror("[node] shared directory");
        return 1;
    }
    server.opts = (thread_server_options_t){
        .bind_ip = "0.0.0.0",
        .port = server.server_cfg.listen_port,
        .max_clients = 64,
        .io_timeout_ms = 5000,
        .backlog = 64
    };
    // Keep the file server running while the user enters tracker commands.
    pthread_t server_thread;
    int rc = pthread_create(&server_thread, NULL, server_main, &server);
    if (rc != 0) {
        errno = rc;
        perror("[node] thread startup");
        return 1;
    }
    printf("[node] Peer server starting at 0.0.0.0:%u (shared: %s), tracker %s:%u\n",
        (unsigned)server.server_cfg.listen_port,server.server_cfg.shared_dir,
        client.tracker_ip,(unsigned)client.tracker_port);
    fflush(stdout);
    // Check for a server startup error before accepting commands.
    struct timespec wait_time = {0, 200000000L};
    nanosleep(&wait_time, NULL);
    if (server_failed) {
        stop_requested = 1;
        pthread_join(server_thread, NULL);
        return 1;
    }
    int ok = 1;
    if (argc>1) {
        if (execute_command(argv + 1, &server) < 0) ok = 0;
    } else {
        puts("[node] Commands: LIST, GET file.track, createtracker ..., updatetracker ..., quit");
        char line[2048];
        while (!stop_requested) {
            printf("node> ");
            fflush(stdout);
            if (!fgets(line, sizeof line, stdin)) break;
            if (!strchr(line, '\n') && !feof(stdin)) {
                int c;
                while ((c = getchar()) != '\n' && c != EOF) {};
                fprintf(stderr, "[node] command too long\n");
                ok = 0;
                continue;
            }
            char *fields[16] = {0};
            int n = command_tokens(line, fields, 16);
            if (n < 0) {
                fprintf(stderr, "[node] too many fields\n");
                ok = 0;
                continue;
            }
            if (!n) continue;
            if (!strcmp(fields[0], "quit") || !strcmp(fields[0], "exit")) break;
            if (execute_command(fields, &server) < 0) ok = 0;
        }
    }
    // Stop the server before exiting the node.
    stop_requested = 1;
    pthread_join(server_thread, NULL);
    return (ok && !server_failed) ? 0 : 1;
}
