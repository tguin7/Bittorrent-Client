#define _POSIX_C_SOURCE 200809L
#include "node_download.h"
#include "net_io.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <openssl/evp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_PEERS 256

// store address and file range
typedef struct 
{
    char host[INET_ADDRSTRLEN];
    uint16_t port;
    uint64_t start, end, timestamp;
} source_peer_t;

//store metadata and peers in tracker 
typedef struct 
{
    char filename[181];
    uint64_t filesize;
    char md5[33];
    source_peer_t peers[MAX_PEERS];
    unsigned peer_count;
} tracker_t;

//check that file is valid
static int is_filename(const char *s) 
{
    size_t len = strlen(s);
    if (len == 0 || len > 180 || s[0] == '.')
    {
        return 0;
    }

    //validate
    for(const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++)
    {
        if (!isalnum(*p) && *p != '.' && *p != '_' && *p != '-') 
        {
            return 0;
        }
    }
    return 1;
}

//convert digits to 64 bit int
static int unsigned_text(const char *s, uint64_t *out) 
{
    if(s == NULL || *s == '\0')
    {
        return 0;
    }

    uint64_t val = 0;

    for(const char *p = s; *p != '\0'; p++) 
    {
        if(!isdigit((unsigned char)*p))
        {
            return 0;
        }

        unsigned digit = (unsigned)(*p - '0');
        if(val > (UINT64_MAX - digit) / 10)
        {
            return 0;
        }

        val = (val * 10) + digit;
    }

    *out = val;
    return 1;
}

//validate MD5 string
static int valid_md5(const char *s)
 {
    if (strlen(s) != 32)
    {
        return 0;
    }

    for(const char *p = s; *p != '\0'; p++)
    {
        if(!isxdigit((unsigned char)*p))
        {
            return 0;
        }
    }

    return 1;
}

//parse one peer record 
static int read_peer_record(char *line,tracker_t *t) 
{
    char *parts[5]; 
    char *p = line;

    //split record to 5 fields
    for(int i = 0; i < 5; i++) 
    {
        parts[i] = p;
        char *sep = strchr(p, ':');

        if(i < 4)
        { 
            if(sep == NULL)
            {
                return -1;
            }

            *sep = '\0';
            p = sep + 1;
        }
        else if(sep != NULL)
        {
             return -1;
        }
    }

    struct in_addr address;
    uint64_t port, start, end, times;

    //reject invalid
    if(inet_pton(AF_INET, parts[0], &address) != 1 ||!unsigned_text(parts[1], &port) || port == 0 || (port  > 65535) ||
        !unsigned_text(parts[2], &start) || !unsigned_text(parts[3], &end) || !unsigned_text(parts[4], &times) || (start > end) || end > t->filesize ||
        t->peer_count == MAX_PEERS)
    { 
        return -1;
    }

    //save valid peer to tracker list
    source_peer_t *entry = &t->peers[t->peer_count++];
    strcpy(entry->host, parts[0]);
    entry->port = (uint16_t)port;
    entry->start = start;
    entry->end = end;
    entry->timestamp = times;

    return 0;
}

// read and val metadata and peer list
static int read_tracker(const char *path,tracker_t *t) 
{
    memset(t, 0, sizeof(*t));

    FILE *file = fopen(path, "r");

    if(!file) 
    {
        perror(path);
        return -1; 
    }

    char *line = NULL;
    size_t capacity = 0;
    ssize_t len;
    int stage = 0, ok = 0;

    //read in order
    while((len = getline(&line, &capacity, file)) != -1) 
    {
        if(len > 8192 || memchr(line, 0, (size_t)len)) 
        {
            break;
        }

        
        while(len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) 
        {
            line[--len] = 0;
        }

        if(len == 0 || line[0] == '#')
        {
            continue;
        }

        if(stage == 0 && strncmp(line,"Filename: ", 10) == 0)
        {
            if(!is_filename(line + 10)) 
            {
                break;
            }

            strcpy(t->filename, line + 10);
            stage = 1;
        } 
        else if(stage == 1 && strncmp(line,"Filesize: ", 10) == 0) {
            if(!unsigned_text(line + 10,&t->filesize) || t->filesize > INT64_MAX) 
            {
                break;
            }
            stage=2;
        } 
        else if(stage == 2 && strncmp(line,"Description:", 12) == 0)
        {
            stage=3;
        }
        else if((stage == 2 || stage == 3) && strncmp(line,"MD5: ",5) == 0) 
        {
            if(!valid_md5(line + 5))
            {
                break;
            }
            strcpy(t->md5, line + 5);
            stage = 4;
        }
        else if(stage == 4) 
        {
            if(read_peer_record(line,t) < 0)
            {
                break;
            }
        } 
        else
        {
            break;
        }
    }

    //file contains all metadata
    if(feof(file) && stage == 4 && (t->filesize == 0 || t->peer_count > 0)) 
    {
        ok = 1;
    }

    if(ferror(file)) 
    {
        ok = 0;
    }

    free(line);
    fclose(file);

    //check that tracker file name matches metadata
    if(ok)
    {
        const char *base = strrchr(path,'/');
        
        if(base != NULL)
        {
            base = base + 1;
        }
        else
        {
            base = path;
        }

        char expected[190];
        snprintf(expected, sizeof(expected), "%s.track", t->filename);

        if(strcmp(base, expected) != 0)
        {
            ok = 0;
        }
    }
    if(!ok) 
    { 
        fprintf(stderr, "[download] Invalid tracker metadata: %s\n", path);
        return -1;
    }

    return 0;
}

//get md5 checksum and store hexadecimal
static int file_md5(const char *path,char hex[33])
{
    FILE *file = fopen(path, "rb");
    if(!file) 
    {
        return -1;
    }

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();

    int ok = ctx && EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
    unsigned char buff[16384];
    size_t n;

    //read file in blocks then add
    while(ok && (n = fread(buff, 1, sizeof(buff), file)) > 0)
    {
        ok = EVP_DigestUpdate(ctx, buff, n);
    }

    if (ferror(file))
    {
        ok = 0;
    }

    unsigned char digest[EVP_MAX_MD_SIZE]; 
    unsigned int len = 0;

    if(ok)
    {
        ok = EVP_DigestFinal_ex(ctx, digest, &len) && len == 16;
    }

    EVP_MD_CTX_free(ctx);
    fclose(file);

    if(!ok)
    {
        return -1;
    }

    //convert checksum to hexadecimal
    for(unsigned i = 0; i < 16; i++)
    {
        snprintf(hex + (2 * i), 3, "%02x", digest[i]);
    }
    return 0;
}

//write until all bytees are saved
static int full_write(int fd, const void *buffer, size_t length, off_t offset)
{
    const unsigned char *p = buffer;
    size_t total = 0;

    while(total < length) 
    {
        ssize_t n = pwrite(fd, p + total, length - total, offset + (off_t)total);
        if(n<0 && errno==EINTR)
        {
            continue;
        }

        if(n <= 0) 
        {
            return -1;
        }

        total += (size_t)n;
    }
    return 0;
}

// choose newest peer
static const source_peer_t *choose_seed(const tracker_t *t) 
{
    const source_peer_t *best = NULL;
    
    //compare peers and skip uncompleted ones
    for (unsigned i = 0; i < t->peer_count; ++i)
    {
        const source_peer_t *p = &t->peers[i];

        if(p->start != 0 || p->end < t->filesize)
        {
            continue;
        }

        if(best == NULL || p->timestamp > best->timestamp)
        {
            best = p;
        }
    }

    return best;
}

// request one chunk from peer and save to disk
static int receive_chunk(const tracker_t *t, const source_peer_t *peer, uint64_t offset, size_t length, int output_fd) 
{
    //open TCP connection
    int socket_fd = net_connect_tcp(peer->host, peer->port, 4000);
    if(socket_fd < 0)
    {
        fprintf(stderr, "[download] Cannot connect to %s:%u: %s\n", peer->host, (unsigned)peer->port, strerror(errno));
        return -1;
    }

    //Build GET request and response header
    char request[256];
    int n = snprintf(request, sizeof request, "<GET %s %" PRIu64 " %zu>\n", t->filename, offset, length);
    char response[64];
    char expected[64];
    snprintf(expected, sizeof expected, "<DATA %zu>\n", length);
    unsigned char bytes[1024];
    int ok = (n > 0 && (size_t)n < sizeof request && net_send_all(socket_fd, request, (size_t)n) == 0 && net_recv_line(socket_fd, response, sizeof response) > 0 &&
             strcmp(response, expected) == 0);
    
    if(ok)
    {
        // receive full chunk
        ok = net_recv_exact(socket_fd, bytes, length) == 0;
    }

    close(socket_fd);

    if(!ok) 
    {
        fprintf(stderr, "[download] Chunk at offset %" PRIu64 " failed\n", offset);
        return -1;
    }

    //Write received chunk at position
    if (full_write(output_fd, bytes, length, (off_t)offset) != 0) 
    {
        perror("[download] writing chunk");
        return -1;
    }

    printf("[download] Received offset=%" PRIu64 " length=%zu\n", offset, length);
    return 0;
}

// download file using peer info
int node_download_from_cache(const char *cache_file, const char *shared_dir) 
{
    // load and val tracker metadata
    tracker_t *tracker = calloc(1, sizeof(*tracker));

    if(!tracker)
    {
        return -1;
    }

    if(read_tracker(cache_file, tracker) != 0)
    {
        free(tracker);
        return -1;
    }

    // build final path and temp path
    char output[4096], partial[4096], actual_md5[33];

    int n = snprintf(output, sizeof output, "%s/%s", shared_dir, tracker->filename);
    if(n < 0 || (size_t)n >= sizeof output)
    {
        free(tracker);
        return -1;
    }

    n = snprintf(partial, sizeof partial, "%s.part", output);
    if(n < 0 || (size_t)n >= sizeof partial) 
    {
        free(tracker);
        return -1;
    }

    // keep existing file when it matches tracker
    struct stat existing;

    if(stat(output, &existing) == 0 && S_ISREG(existing.st_mode) && existing.st_size >= 0 && (uint64_t)existing.st_size == tracker->filesize &&
        file_md5(output, actual_md5) == 0 && strcasecmp(actual_md5, tracker->md5) == 0) 
    {
        printf("[download] Already complete: %s\n", output);
        if(unlink(cache_file) != 0)
        {
            perror("[download] cache cleanup");
        }

        free(tracker);
        return 0;
    }

    // find seed
    const source_peer_t *seed = choose_seed(tracker);
    if(tracker->filesize != 0 && !seed)
    {
        fprintf(stderr, "[download] No peer has the complete file; try again with updated tracker data\n");
        free(tracker);
        return -1;
    }

    //start new part file
    int output_fd = open(partial, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if(output_fd < 0) 
    {
        perror("[download] opening partial file");
        free(tracker);
        return -1;
    }

    if(seed)
    {
        printf("[download] Starting %s (%" PRIu64 " bytes) from %s:%u\n", tracker->filename, tracker->filesize, seed->host, (unsigned)seed->port);
    }

    //download chunks
    int ok = 1;
    uint64_t offset = 0;

    while(offset < tracker->filesize)
    {
        uint64_t remaining = tracker->filesize - offset;
        size_t length;

        if(remaining > 1024)
        {
            length = 1024;
        }
        else
        {
            length = (size_t)remaining; 
        }

        if(receive_chunk(tracker, seed, offset, length, output_fd) != 0)
        {
            ok = 0;
            break;
        }

        offset += length;
    }
    
    //flush downloaded data
    if(ok && fsync(output_fd) != 0)
    {
        perror("[download] sync");
        ok = 0;
    }

    if(close(output_fd) != 0) 
    {
        ok = 0;
    }

    // reject download that do not match checksum
    if(ok && (file_md5(partial, actual_md5) != 0 || strcasecmp(actual_md5, tracker->md5) != 0)) 
    {
        fprintf(stderr, "[download] File MD5 mismatch\n");
        ok = 0;
    }

    //move temp file to final
    if(ok && rename(partial, output) != 0) 
    {
        perror("[download] rename");
        ok = 0;
    }

    //remove incomplete data
    if(!ok) 
    {
        unlink(partial);
        fprintf(stderr, "[download] Download failed; cached tracker metadata retained\n");
        free(tracker);
        return -1;
    }

    //finished file ready
    if(unlink(cache_file) != 0)
    {
        perror("[download] cache cleanup");
    }

    printf("[download] COMPLETE %s (MD5 %s)\n", output, actual_md5);
    free(tracker);

    return 0;
}
