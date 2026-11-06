/* TEST-ONLY server exercising Member 4's generic thread/transport framework.
 * Not a tracker or production file-serving implementation.
 */
#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "net_io.h"
#include "thread_server.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopping = 0;
static void request_shutdown(int signal_number) { (void)signal_number; stopping = 1; }

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t barrier_ready;
    int barrier_count;
} fixture_state_t;

static int send_text(int fd, const char *text) {
    return net_send_all(fd, text, strlen(text));
}

static void handle_test_connection(int fd, void *context) {
    fixture_state_t *state = context;
    char request[256];
    if (net_recv_line(fd, request, sizeof request) < 0) return;
    fprintf(stderr, "TEST worker %lu request %s", (unsigned long)pthread_self(), request);
    fflush(stderr);

    char name[100], expected[256];
    if (sscanf(request, "<PING %99[a-zA-Z0-9_-]>", name) == 1) {
        snprintf(expected, sizeof expected, "<PING %s>\n", name);
        if (!strcmp(request, expected)) {
            snprintf(expected, sizeof expected, "<PONG %s>\n", name);
            (void)send_text(fd, expected);
            return;
        }
    }
    unsigned count = 0;
    int consumed = 0;
    if (sscanf(request, "<ECHO %u>%n", &count, &consumed) == 1 &&
        consumed == (int)strlen(request) - 1) {
        if (count > 1024) { (void)send_text(fd, "<ERROR too-large>\n"); return; }
        unsigned char data[1024];
        if (net_recv_exact(fd, data, count) < 0) return;
        snprintf(expected, sizeof expected, "<DATA %u>\n", count);
        if (send_text(fd, expected) == 0) (void)net_send_all(fd, data, count);
        return;
    }
    if (!strcmp(request, "<BARRIER>\n")) {
        pthread_mutex_lock(&state->mutex);
        state->barrier_count++;
        if (state->barrier_count >= 4) pthread_cond_broadcast(&state->barrier_ready);
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += 3;
        while (state->barrier_count < 4) {
            if (pthread_cond_timedwait(&state->barrier_ready,
                                       &state->mutex, &until) == ETIMEDOUT) break;
        }
        int succeeded = state->barrier_count >= 4;
        pthread_mutex_unlock(&state->mutex);
        (void)send_text(fd, succeeded ? "<BARRIER ok>\n" : "<BARRIER timeout>\n");
        return;
    }
    if (!strcmp(request, "<HANG>\n")) {
        if (send_text(fd, "<WAITING>\n") < 0) return;
        unsigned char dummy;
        if (net_recv_exact(fd, &dummy, 1) < 0) (void)send_text(fd, "<TIMEDOUT>\n");
        return;
    }
    (void)send_text(fd, "<ERROR invalid>\n");
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s serverThreadConfig.cfg [max-clients]\n", argv[0]);
        return 2;
    }
    server_config_t cfg;
    if (config_read_server(argv[1], &cfg) < 0) { perror("config"); return 2; }
    unsigned max_clients = 8;
    if (argc == 3) {
        char *end = NULL;
        unsigned long value = strtoul(argv[2], &end, 10);
        if (!argv[2][0] || !end || *end || !value || value > 128) return 2;
        max_clients = (unsigned)value;
    }
    struct sigaction sa = {0};
    sa.sa_handler = request_shutdown;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    fixture_state_t state = {0};
    if (pthread_mutex_init(&state.mutex, NULL) != 0 ||
        pthread_cond_init(&state.barrier_ready, NULL) != 0) return 2;
    thread_server_options_t options = {
        .bind_ip = "127.0.0.1",
        .port = cfg.listen_port,
        .max_clients = max_clients,
        .io_timeout_ms = 1500,
        .backlog = 16,
    };
    fprintf(stderr, "TEST server listening 127.0.0.1:%u (max=%u)\n",
            (unsigned)cfg.listen_port, max_clients);
    int rc = thread_server_run(&options, handle_test_connection, &state, &stopping);
    if (rc < 0) perror("thread_server_run");
    pthread_cond_destroy(&state.barrier_ready);
    pthread_mutex_destroy(&state.mutex);
    return rc == 0 ? 0 : 1;
}
