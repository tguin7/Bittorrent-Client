#ifndef MEMBER4_DOWNLOAD_POOL_H
#define MEMBER4_DOWNLOAD_POOL_H

#include <stddef.h>
#include <stdint.h>

// store info about a chunk to download
typedef struct 
{
    uint64_t offset;
    uint16_t length;
} download_chunk_t;


// callback to used chunk
typedef int (*download_chunk_callback_t)(const download_chunk_t *chunk, unsigned attempt, void *context);

// run dowload using multiple threads
int download_pool_run(const download_chunk_t *jobs, size_t job_count, unsigned worker_count, unsigned max_attempts, download_chunk_callback_t callback, void *context,
                      unsigned char *success);

#endif
