#define _POSIX_C_SOURCE 200809L
#include "thread_server.h"
#include "net_io.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// Shared state for tracking active worker threads.
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t idle;
    unsigned active;
    thread_connection_handler_t handler;
    void *context;
} server_state_t;

typedef struct {
    server_state_t *server;
    int fd;
} worker_args_t;

static void *run_worker(void *arg) {
    worker_args_t *worker = arg;
    server_state_t *server = worker->server;
    int fd = worker->fd;
    free(worker);

    // The server closes the client socket after the handler returns.
    server->handler(fd, server->context);
    close(fd);

    // Update the connection count while holding the mutex.
    pthread_mutex_lock(&server->mutex);
    --server->active;
    pthread_cond_signal(&server->idle);
    pthread_mutex_unlock(&server->mutex);
    return NULL;
}

static int create_listener(const thread_server_options_t *options, int backlog) {
    const char *host = options->bind_ip ? options->bind_ip : "0.0.0.0";
    char service[6];
    snprintf(service, sizeof service, "%u", (unsigned)options->port);
    struct addrinfo hint = {0}, *addrs = NULL;
    hint.ai_family = AF_UNSPEC;
    hint.ai_socktype = SOCK_STREAM;
    hint.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    if (getaddrinfo(host, service, &hint, &addrs) != 0) {
        errno = EINVAL;
        return -1;
    }
    int listener = -1;
    int last_error = EADDRNOTAVAIL;
    // Try the available addresses until a socket can bind and listen.
    for (struct addrinfo *p = addrs; p; p = p->ai_next) {
        int fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) {
            last_error = errno;
            continue;
        }
        int reuse = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse) < 0 ||
            bind(fd, p->ai_addr, p->ai_addrlen) < 0 || listen(fd, backlog) < 0) {
            last_error = errno;
            close(fd);
            continue;
        }
        int flags = fcntl(fd, F_GETFD);
        if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
            last_error = errno;
            close(fd);
            continue;
        }
        listener = fd;
        break;
    }
    freeaddrinfo(addrs);
    if (listener < 0) errno = last_error;
    return listener;
}

int thread_server_run(const thread_server_options_t *options,
                      thread_connection_handler_t handler,
                      void *context,
                      const volatile sig_atomic_t *stop_requested) {
    if (!options || !handler || !stop_requested || options->io_timeout_ms < 0 ||
        options->backlog < 0) {
        errno = EINVAL;
        return -1;
    }
    unsigned max_clients = options->max_clients ? options->max_clients : 32;
    int io_timeout = options->io_timeout_ms ? options->io_timeout_ms : 5000;
    int listener = create_listener(options, options->backlog ? options->backlog : 64);
    if (listener < 0) return -1;

    server_state_t state = {0};
    state.handler = handler;
    state.context = context;
    int rc = pthread_mutex_init(&state.mutex, NULL);
    if (rc != 0) {
        close(listener);
        errno = rc;
        return -1;
    }
    rc = pthread_cond_init(&state.idle, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&state.mutex);
        close(listener);
        errno = rc;
        return -1;
    }
    pthread_attr_t attributes;
    rc = pthread_attr_init(&attributes);
    int attr_initialized = rc == 0;
    if (attr_initialized) rc = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    if (rc != 0) {
        if (attr_initialized) pthread_attr_destroy(&attributes);
        pthread_cond_destroy(&state.idle);
        pthread_mutex_destroy(&state.mutex);
        close(listener);
        errno = rc;
        return -1;
    }

    int error_code = 0;
    while (!*stop_requested) {
        struct pollfd pfd = {listener, POLLIN, 0};
        // Check periodically so the server can stop when requested.
        int ready = poll(&pfd, 1, 100);
        if (ready < 0) {
            if (errno == EINTR) continue;
            error_code = errno;
            break;
        }
        if (ready == 0) continue;
        if (pfd.revents & (POLLERR | POLLNVAL | POLLHUP)) {
            error_code = EIO;
            break;
        }
        if (!(pfd.revents & POLLIN)) continue;
        int client = accept(listener, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            error_code = errno;
            break;
        }
        int flags = fcntl(client, F_GETFD);
        if (flags < 0 || fcntl(client, F_SETFD, flags | FD_CLOEXEC) < 0 ||
            net_set_socket_timeouts(client, io_timeout) < 0) {
            close(client);
            continue;
        }
        pthread_mutex_lock(&state.mutex);
        if (state.active >= max_clients) {
            pthread_mutex_unlock(&state.mutex);
            // Too many connections; close this one.
            close(client);
            continue;
        }
        ++state.active; // Count the client before starting its thread.
        pthread_mutex_unlock(&state.mutex);

        // Pass the client socket and server state to the worker thread.
        worker_args_t *args = malloc(sizeof *args);
        if (!args) {
            close(client);
            pthread_mutex_lock(&state.mutex);
            --state.active;
            pthread_cond_signal(&state.idle);
            pthread_mutex_unlock(&state.mutex);
            continue;
        }
        args->fd = client;
        args->server = &state;
        pthread_t thread;
        rc = pthread_create(&thread, &attributes, run_worker, args);
        if (rc != 0) {
            free(args);
            close(client);
            pthread_mutex_lock(&state.mutex);
            --state.active;
            pthread_cond_signal(&state.idle);
            pthread_mutex_unlock(&state.mutex);
            continue;
        }
    }
    close(listener);
    pthread_attr_destroy(&attributes);

    // Wait for detached workers before destroying the shared state.
    pthread_mutex_lock(&state.mutex);
    while (state.active > 0) pthread_cond_wait(&state.idle, &state.mutex);
    pthread_mutex_unlock(&state.mutex);
    pthread_cond_destroy(&state.idle);
    pthread_mutex_destroy(&state.mutex);
    if (error_code) {
        errno = error_code;
        return -1;
    }
    return 0;
}
