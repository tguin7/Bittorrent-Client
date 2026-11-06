/* C-level tests for independent four-thread chunk scheduler (no tracker needed). */
#define _POSIX_C_SOURCE 200809L
#include "download_pool.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    unsigned active;
    unsigned max_active;
    unsigned arrivals;
    unsigned calls;
    unsigned attempt_two;
} context_t;

static int callback(const download_chunk_t *job, unsigned attempt, void *ptr) {
    context_t *ctx = ptr;
    pthread_mutex_lock(&ctx->mutex);
    ctx->active++;
    if (ctx->active > ctx->max_active) ctx->max_active = ctx->active;
    ctx->calls++;
    if (attempt == 2) ctx->attempt_two++;
    /* Four simultaneous requests must be possible. */
    if (job->offset < 4 && attempt == 1) {
        ctx->arrivals++;
        if (ctx->arrivals == 4) pthread_cond_broadcast(&ctx->cond);
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += 2;
        while (ctx->arrivals < 4) {
            if (pthread_cond_timedwait(&ctx->cond, &ctx->mutex, &until) == ETIMEDOUT)
                break;
        }
        if (ctx->arrivals != 4) {
            ctx->active--;
            pthread_mutex_unlock(&ctx->mutex);
            return 1;
        }
    }
    ctx->active--;
    pthread_mutex_unlock(&ctx->mutex);
    /* Offset 10: transient failure, then succeeds; offset 11: always fails. */
    if (job->offset == 10 && attempt == 1) return 1;
    if (job->offset == 11) return 1;
    return 0;
}

#define REQUIRE(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); return 1; \
} } while (0)

int main(void) {
    context_t ctx = {0};
    REQUIRE(pthread_mutex_init(&ctx.mutex, NULL) == 0);
    REQUIRE(pthread_cond_init(&ctx.cond, NULL) == 0);
    download_chunk_t jobs[12];
    unsigned char status[12];
    for (unsigned i = 0; i < 12; ++i) {
        jobs[i].offset = i;
        jobs[i].length = 1;
    }
    REQUIRE(download_pool_run(jobs, 12, 4, 3, callback, &ctx, status) == 1);
    REQUIRE(ctx.max_active == 4);
    REQUIRE(ctx.arrivals == 4);
    REQUIRE(ctx.calls == 15); /* 12 first attempts + 1 retry on 10 + 2 on 11 */
    REQUIRE(ctx.attempt_two == 2);
    for (unsigned i = 0; i < 12; ++i) REQUIRE(status[i] == (i == 11 ? 0 : 1));
    jobs[1].offset = 0; /* overlap with jobs[0] */
    REQUIRE(download_pool_run(jobs, 12, 4, 1, callback, &ctx, status) == -1);
    REQUIRE(errno == EINVAL);
    jobs[1].offset = 1;
    jobs[2].length = 1025;
    REQUIRE(download_pool_run(jobs, 12, 4, 1, callback, &ctx, status) == -1);
    REQUIRE(errno == EINVAL);
    jobs[2].length = 1;
    REQUIRE(download_pool_run(jobs, 0, 4, 1, callback, &ctx, status) == 0);
    REQUIRE(download_pool_run(jobs, 12, 0, 1, callback, &ctx, status) == -1);
    REQUIRE(errno == EINVAL);
    pthread_cond_destroy(&ctx.cond);
    pthread_mutex_destroy(&ctx.mutex);
    puts("Member 4 download pool tests passed (4 concurrent jobs, retry, failure, nonoverlap).");
    return 0;
}
