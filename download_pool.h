#ifndef MEMBER4_DOWNLOAD_POOL_H
#define MEMBER4_DOWNLOAD_POOL_H

#include <stddef.h>
#include <stdint.h>

/* For the agreed protocol, bytes [offset, offset + length) are requested.
 * No two jobs passed to download_pool_run may overlap; length <= 1024.
 * Members 2/3 create these jobs after parsing peer availability and the
 * local completed-chunk record. Completed chunks can be omitted for resume.
 */
typedef struct {
    uint64_t offset;
    uint16_t length;
} download_chunk_t;

/* Called from one of up to worker_count POSIX threads. 'attempt' is 1-based.
 * Return 0 for success, nonzero for failure (the scheduler retries).
 * This callback is responsible for selecting peers, transferring chunks,
 * writing to the correct file offset (e.g. pwrite), and synchronization of
 * its own shared objects. It must use bounded network timeouts.
 */
typedef int (*download_chunk_callback_t)(const download_chunk_t *chunk,
                                          unsigned attempt, void *context);

/* Execute jobs concurrently, max one active worker per job, with retries.
 * 'success' is an output array of job_count bytes (1=success, 0=failed).
 * Valid worker_count: 1..64; valid max_attempts: 1..32.
 * Function does not return until all started worker threads are joined.
 * Return 0 if every job succeeds; 1 if any job is incomplete; -1 for invalid
 * arguments, thread startup failure or other setup errors (errno set).
 * On -1, 'success' can still contain a partial record; caller must review it.
 * The library does NOT write files or assume any peer protocol.
 */
int download_pool_run(const download_chunk_t *jobs, size_t job_count,
                      unsigned worker_count, unsigned max_attempts,
                      download_chunk_callback_t callback, void *context,
                      unsigned char *success);

#endif
