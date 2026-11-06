#ifndef MEMBER4_THREAD_SERVER_H
#define MEMBER4_THREAD_SERVER_H

#include <signal.h>
#include <stdint.h>

/* The handler owns application protocol parsing and replies, but NOT client_fd.
 * The framework always closes the accepted socket after the handler returns.
 * Handler context is shared across connections; owning module must protect
 * mutable state (e.g., .track files) with its own mutexes.
 * Each connection is served in one detached POSIX thread.
 */
typedef void (*thread_connection_handler_t)(int client_fd, void *context);

typedef struct {
    const char *bind_ip;    /* NULL -> IPv4 0.0.0.0; numeric IPv4/IPv6 otherwise */
    uint16_t port;          /* 0 is allowed for local tests (ephemeral listener) */
    unsigned max_clients;   /* 0 -> 32 concurrent workers; excess clients closed */
    int io_timeout_ms;      /* 0 -> 5000ms; applies to accepted sockets */
    int backlog;            /* 0 -> 64; listen() pending connection queue */
} thread_server_options_t;

/* Blocks in accept/poll loop until *stop_requested becomes nonzero.
 * stop_requested must point to a valid volatile sig_atomic_t for the entire run.
 * Caller may set it in a SIGINT/SIGTERM handler (only signal-safe flag write).
 * Closes listener and waits for active handlers before returning.
 * Returns 0 on clean stop, or -1 with errno on setup/accept/poll failure.
 * Handlers must use finite timeouts / return promptly to allow shutdown.
 */
int thread_server_run(const thread_server_options_t *options,
                      thread_connection_handler_t handler,
                      void *context,
                      const volatile sig_atomic_t *stop_requested);

#endif
