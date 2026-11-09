#define _POSIX_C_SOURCE 200809L
#include "download_pool.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const download_chunk_t *jobs;
    size_t job_count;
    size_t next_job;
    unsigned attempts;
    download_chunk_callback_t callback;
    void *context;
    unsigned char *success;
    pthread_mutex_t mutex;
} pool_state_t;

static int compare_chunks(const void *a, const void *b) {
    const download_chunk_t *x = a, *y = b;
    if (x->offset < y->offset) return -1;
    if (x->offset > y->offset) return 1;
    return 0;
}

/* Validate no overlapping jobs: required for mutual exclusion by offset.
 * Validate a sorted COPY so the caller's job ordering is preserved.
 */
static int verify_jobs(const download_chunk_t *jobs, size_t count) {
    if (count > SIZE_MAX / sizeof *jobs) { errno = EOVERFLOW; return -1; }
    download_chunk_t *copy = malloc(count * sizeof *copy);
    if (!copy) return -1;
    for (size_t i = 0; i < count; ++i) {
        if (!jobs[i].length || jobs[i].length > 1024 ||
            jobs[i].offset > UINT64_MAX - jobs[i].length) {
            free(copy);
            errno = EINVAL;
            return -1;
        }
        copy[i] = jobs[i];
    }
    qsort(copy, count, sizeof *copy, compare_chunks);
    for (size_t i = 1; i < count; ++i) {
        if (copy[i-1].offset + copy[i-1].length > copy[i].offset) {
            free(copy);
            errno = EINVAL;
            return -1;
        }
    }
    free(copy);
    return 0;
}

static void *download_worker(void *arg) {
    pool_state_t *pool = arg;
    for (;;) {
        pthread_mutex_lock(&pool->mutex);
        if (pool->next_job >= pool->job_count) {
            pthread_mutex_unlock(&pool->mutex);
            break;
        }
        size_t index = pool->next_job++;
        pthread_mutex_unlock(&pool->mutex);

        for (unsigned attempt = 1; attempt <= pool->attempts; ++attempt) {
            if (pool->callback(&pool->jobs[index], attempt, pool->context) == 0) {
                pool->success[index] = 1;
                break;
            }
        }
    }
    return NULL;
}

int download_pool_run(const download_chunk_t *jobs, size_t job_count,
                      unsigned worker_count, unsigned max_attempts,
                      download_chunk_callback_t callback, void *context,
                      unsigned char *success) {
    if (worker_count == 0 || worker_count > 64 || max_attempts == 0 || max_attempts > 32 ||
        !callback || (job_count && (!jobs || !success))) {
        errno = EINVAL;
        return -1;
    }
    if (job_count == 0) return 0;
    if (verify_jobs(jobs, job_count) < 0) return -1;
    memset(success, 0, job_count);
    if (worker_count > job_count) worker_count = (unsigned)job_count;

    pool_state_t pool = {
        .jobs = jobs, .job_count = job_count, .next_job = 0,
        .attempts = max_attempts, .callback = callback,
        .context = context, .success = success,
    };
    int rc = pthread_mutex_init(&pool.mutex, NULL);
    if (rc) { errno = rc; return -1; }
    pthread_t threads[64];
    unsigned started = 0;
    int create_error = 0;
    for (; started < worker_count; ++started) {
        rc = pthread_create(&threads[started], NULL, download_worker, &pool);
        if (rc != 0) { create_error = rc; break; }
    }
    for (unsigned i = 0; i < started; ++i) pthread_join(threads[i], NULL);
    pthread_mutex_destroy(&pool.mutex);
    if (create_error) { errno = create_error; return -1; }
    for (size_t i = 0; i < job_count; ++i) if (!success[i]) return 1;
    return 0;
}
