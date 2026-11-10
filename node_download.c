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

typedef struct {
    char host[INET_ADDRSTRLEN];
    uint16_t port;
    uint64_t start, end, timestamp;
} source_peer_t;
typedef struct {
    char filename[181];
    uint64_t filesize;
    char md5[33];
    source_peer_t peers[MAX_PEERS];
    unsigned peer_count;
} tracker_t;

static int is_filename(const char *s) {
    size_t n=strlen(s);
    if (!n || n>180 || s[0]=='.') return 0;
    for (const unsigned char *p=(const unsigned char *)s; *p; ++p)
        if (!isalnum(*p) && *p!='.' && *p!='_' && *p!='-') return 0;
    return 1;
}

static int unsigned_text(const char *s, uint64_t *out) {
    if (!s || !*s) return 0;
    uint64_t x=0;
    for (; *s; ++s) {
        if (!isdigit((unsigned char)*s) || x > (UINT64_MAX - (unsigned)(*s-'0'))/10) return 0;
        x=x*10+(unsigned)(*s-'0');
    }
    *out=x;return 1;
}

static int valid_md5(const char *s) {
    if (strlen(s)!=32) return 0;
    for (;*s;s++) if (!isxdigit((unsigned char)*s)) return 0;
    return 1;
}

static int read_peer_record(char *line,tracker_t *t) {
    char *parts[5];
    char *p = line;
    for (int i = 0; i < 5; i++) {
        parts[i] = p;
        char *sep = strchr(p, ':');
        if (i < 4) {
            if (!sep) return -1;
            *sep = '\0';
            p = sep + 1;
        } else if (sep) return -1;
    }
    struct in_addr a;
    uint64_t port, start, end, ts;
    if (inet_pton(AF_INET,parts[0],&a)!=1 ||
        !unsigned_text(parts[1],&port) || port==0 || port>65535 ||
        !unsigned_text(parts[2],&start) || !unsigned_text(parts[3],&end) ||
        !unsigned_text(parts[4],&ts) || start>end || end>t->filesize ||
        t->peer_count==MAX_PEERS) return -1;
    source_peer_t *entry = &t->peers[t->peer_count++];
    strcpy(entry->host, parts[0]);
    entry->port = (uint16_t)port;
    entry->start = start;
    entry->end = end;
    entry->timestamp = ts;
    return 0;
}

// The .track file contains file information followed by the available peers.
static int read_tracker(const char *path, tracker_t *t) {
    memset(t, 0, sizeof *t);
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        return -1;
    }
    char *line = NULL;
    size_t capacity = 0;
    ssize_t n;
    int stage = 0, ok = 0;
    while ((n = getline(&line, &capacity, f)) != -1) {
        if (n > 8192 || memchr(line, 0, (size_t)n)) break;
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        if (!n || line[0]=='#') continue;
        if (stage==0 && !strncmp(line,"Filename: ",10)) {
            if (!is_filename(line+10)) break;
            strcpy(t->filename, line + 10);
            stage = 1;
        } else if (stage==1 && !strncmp(line,"Filesize: ",10)) {
            if (!unsigned_text(line+10,&t->filesize) || t->filesize>INT64_MAX) break;
            stage = 2;
        } else if (stage==2 && !strncmp(line,"Description:",12)) stage=3;
        else if ((stage==2||stage==3) && !strncmp(line,"MD5: ",5)) {
            if (!valid_md5(line+5)) break;
            strcpy(t->md5, line + 5);
            stage = 4;
        } else if (stage==4) {
            if (read_peer_record(line,t)<0) break;
        } else break;
    }
    if (feof(f) && stage==4 && (t->filesize==0 || t->peer_count>0)) ok=1;
    if (ferror(f)) ok=0;
    free(line);
    fclose(f);
    if (ok) {
        const char *base=strrchr(path,'/');base=base?base+1:path;
        char expected[190];
        snprintf(expected,sizeof expected,"%s.track",t->filename);
        if (strcmp(base,expected)!=0) ok=0;
    }
    if (!ok) { fprintf(stderr,"[download] Invalid tracker metadata: %s\n",path);return -1; }
    return 0;
}

static int file_md5(const char *path, char hex[33]) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    int ok = ctx && EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
    unsigned char buf[16384];
    size_t n;
    while (ok && (n=fread(buf,1,sizeof buf,f))>0)
        ok=EVP_DigestUpdate(ctx,buf,n);
    if (ferror(f)) ok=0;
    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int len=0;
    if (ok) ok=EVP_DigestFinal_ex(ctx,digest,&len) && len==16;
    EVP_MD_CTX_free(ctx);
    fclose(f);
    if (!ok) return -1;
    for (unsigned i=0;i<16;i++) snprintf(hex+2*i,3,"%02x",digest[i]);
    return 0;
}

// pwrite can write fewer bytes than requested, so keep going until done.
static int full_write(int fd, const void *buffer, size_t length, off_t offset) {
    const unsigned char *p = buffer;
    size_t total = 0;
    while (total < length) {
        ssize_t n = pwrite(fd, p + total, length - total, offset + (off_t)total);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        total += (size_t)n;
    }
    return 0;
}
// Use a peer with the full file. If several qualify, pick the newest entry.
static const source_peer_t *choose_seed(const tracker_t *t) {
    const source_peer_t *best = NULL;
    for (unsigned i = 0; i < t->peer_count; ++i) {
        const source_peer_t *p = &t->peers[i];
        if (p->start != 0 || p->end < t->filesize) continue;
        if (!best || p->timestamp > best->timestamp) best = p;
    }
    return best;
}

// Request one chunk per connection. net_recv_exact handles partial reads.
static int receive_chunk(const tracker_t *t, const source_peer_t *peer,
                         uint64_t offset, size_t length, int output_fd) {
    int socket_fd = net_connect_tcp(peer->host, peer->port, 4000);
    if (socket_fd < 0) {
        fprintf(stderr, "[download] Cannot connect to %s:%u: %s\n",
                peer->host, (unsigned)peer->port, strerror(errno));
        return -1;
    }

    char request[256];
    int n = snprintf(request, sizeof request, "<GET %s %" PRIu64 " %zu>\n",
                     t->filename, offset, length);
    // The peer sends a DATA header before the raw chunk bytes.
    char response[64];
    char expected[64];
    snprintf(expected, sizeof expected, "<DATA %zu>\n", length);

    int ok = n > 0 && (size_t)n < sizeof request &&
             net_send_all(socket_fd, request, (size_t)n) == 0 &&
             net_recv_line(socket_fd, response, sizeof response) > 0 &&
             strcmp(response, expected) == 0;
    unsigned char bytes[1024];
    if (ok) ok = net_recv_exact(socket_fd, bytes, length) == 0;
    close(socket_fd);

    if (!ok) {
        fprintf(stderr, "[download] Chunk at offset %" PRIu64 " failed\n", offset);
        return -1;
    }
    if (full_write(output_fd, bytes, length, (off_t)offset) != 0) {
        perror("[download] writing chunk");
        return -1;
    }
    printf("[download] Received offset=%" PRIu64 " length=%zu\n", offset, length);
    return 0;
}

int node_download_from_cache(const char *cache_file, const char *shared_dir) {
    tracker_t *tracker = calloc(1, sizeof *tracker);
    if (!tracker) return -1;
    if (read_tracker(cache_file, tracker) != 0) {
        free(tracker);
        return -1;
    }

    char output[4096], partial[4096], actual_md5[33];
    int n = snprintf(output, sizeof output, "%s/%s", shared_dir, tracker->filename);
    if (n < 0 || (size_t)n >= sizeof output) {
        free(tracker);
        return -1;
    }
    n = snprintf(partial, sizeof partial, "%s.part", output);
    if (n < 0 || (size_t)n >= sizeof partial) {
        free(tracker);
        return -1;
    }

    // Skip the download if the local file already matches.
    struct stat existing;
    if (stat(output, &existing) == 0 && S_ISREG(existing.st_mode) &&
        existing.st_size >= 0 && (uint64_t)existing.st_size == tracker->filesize &&
        file_md5(output, actual_md5) == 0 &&
        strcasecmp(actual_md5, tracker->md5) == 0) {
        printf("[download] Already complete: %s\n", output);
        if (unlink(cache_file) != 0) perror("[download] cache cleanup");
        free(tracker);
        return 0;
    }

    const source_peer_t *seed = choose_seed(tracker);
    if (tracker->filesize != 0 && !seed) {
        fprintf(stderr, "[download] No peer has the complete file; try again with updated tracker data\n");
        free(tracker);
        return -1;
    }

    // Start from the beginning if a previous .part file was left behind.
    int output_fd = open(partial, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (output_fd < 0) {
        perror("[download] opening partial file");
        free(tracker);
        return -1;
    }
    if (seed) {
        printf("[download] Starting %s (%" PRIu64 " bytes) from %s:%u\n",
               tracker->filename, tracker->filesize, seed->host, (unsigned)seed->port);
    }

    // Download one chunk at a time, with no request larger than 1024 bytes.
    int ok = 1;
    for (uint64_t offset = 0; offset < tracker->filesize;) {
        uint64_t remaining = tracker->filesize - offset;
        size_t length = (size_t)(remaining > 1024 ? 1024 : remaining);
        if (receive_chunk(tracker, seed, offset, length, output_fd) != 0) {
            ok = 0;
            break;
        }
        offset += length;
    }
    if (ok && fsync(output_fd) != 0) {
        perror("[download] sync");
        ok = 0;
    }
    if (close(output_fd) != 0) ok = 0;

    // Check the full file before renaming .part to the real filename.
    if (ok && (file_md5(partial, actual_md5) != 0 ||
               strcasecmp(actual_md5, tracker->md5) != 0)) {
        fprintf(stderr, "[download] File MD5 mismatch\n");
        ok = 0;
    }
    if (ok && rename(partial, output) != 0) {
        perror("[download] rename");
        ok = 0;
    }
    if (!ok) {
        // Remove the unfinished file but keep the tracker info for another attempt.
        unlink(partial);
        fprintf(stderr, "[download] Download failed; cached tracker metadata retained\n");
        free(tracker);
        return -1;
    }

    if (unlink(cache_file) != 0) perror("[download] cache cleanup");
    printf("[download] COMPLETE %s (MD5 %s)\n", output, actual_md5);
    free(tracker);
    return 0;
}
