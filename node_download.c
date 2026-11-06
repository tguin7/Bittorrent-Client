#define _POSIX_C_SOURCE 200809L
#include "node_download.h"
#include "net_io.h"
#include "download_pool.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_PEERS 256
#define MAX_CHUNKS 1048576U
#define STATE_HEADER 128

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
typedef struct {
    const tracker_t *tracker;
    int file_fd, state_fd;
    pthread_mutex_t log_mutex;
    size_t completed;
} transfer_ctx_t;

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
    char *parts[5]; char *p=line;
    for (int i=0;i<5;i++) {
        parts[i]=p;
        char *sep=strchr(p,':');
        if (i<4) { if (!sep) return -1; *sep='\0';p=sep+1; }
        else if (sep) return -1;
    }
    struct in_addr a;
    uint64_t port, start, end, ts;
    if (inet_pton(AF_INET,parts[0],&a)!=1 ||
        !unsigned_text(parts[1],&port) || port==0 || port>65535 ||
        !unsigned_text(parts[2],&start) || !unsigned_text(parts[3],&end) ||
        !unsigned_text(parts[4],&ts) || start>end || end>t->filesize ||
        t->peer_count==MAX_PEERS) return -1;
    source_peer_t *entry=&t->peers[t->peer_count++];
    strcpy(entry->host,parts[0]);entry->port=(uint16_t)port;
    entry->start=start;entry->end=end;entry->timestamp=ts;
    return 0;
}
static int read_tracker(const char *path,tracker_t *t) {
    memset(t,0,sizeof *t);
    FILE *f=fopen(path,"r");
    if (!f) { perror(path);return -1; }
    char *line=NULL;size_t capacity=0;ssize_t n;int stage=0,ok=0;
    while ((n=getline(&line,&capacity,f))!=-1) {
        if (n>8192 || memchr(line,0,(size_t)n)) break;
        while (n>0 && (line[n-1]=='\n'||line[n-1]=='\r')) line[--n]=0;
        if (!n || line[0]=='#') continue;
        if (stage==0 && !strncmp(line,"Filename: ",10)) {
            if (!is_filename(line+10)) break;
            strcpy(t->filename,line+10);stage=1;
        } else if (stage==1 && !strncmp(line,"Filesize: ",10)) {
            if (!unsigned_text(line+10,&t->filesize) || t->filesize>INT64_MAX) break;
            stage=2;
        } else if (stage==2 && !strncmp(line,"Description:",12)) stage=3;
        else if ((stage==2||stage==3) && !strncmp(line,"MD5: ",5)) {
            if (!valid_md5(line+5)) break;
            strcpy(t->md5,line+5);stage=4;
        } else if (stage==4) {
            if (read_peer_record(line,t)<0) break;
        } else break;
    }
    if (feof(f) && stage==4 && (t->filesize==0 || t->peer_count>0)) ok=1;
    if (ferror(f)) ok=0;
    free(line);fclose(f);
    if (ok) {
        const char *base=strrchr(path,'/');base=base?base+1:path;
        char expected[190];
        snprintf(expected,sizeof expected,"%s.track",t->filename);
        if (strcmp(base,expected)!=0) ok=0;
    }
    if (!ok) { fprintf(stderr,"[download] Invalid tracker metadata: %s\n",path);return -1; }
    return 0;
}
static int file_md5(const char *path,char hex[33]) {
    FILE *f=fopen(path,"rb");if (!f) return -1;
    EVP_MD_CTX *ctx=EVP_MD_CTX_new();
    int ok=ctx && EVP_DigestInit_ex(ctx,EVP_md5(),NULL);
    unsigned char buf[16384];size_t n;
    while (ok && (n=fread(buf,1,sizeof buf,f))>0)
        ok=EVP_DigestUpdate(ctx,buf,n);
    if (ferror(f)) ok=0;
    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int len=0;
    if (ok) ok=EVP_DigestFinal_ex(ctx,digest,&len) && len==16;
    EVP_MD_CTX_free(ctx);fclose(f);
    if (!ok) return -1;
    for (unsigned i=0;i<16;i++) snprintf(hex+2*i,3,"%02x",digest[i]);
    return 0;
}
static int full_write(int fd,const void *buffer,size_t length,off_t offset) {
    const unsigned char *p=buffer;size_t total=0;
    while (total<length) {
        ssize_t n=pwrite(fd,p+total,length-total,offset+(off_t)total);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) return -1;
        total+=(size_t)n;
    }
    return 0;
}
static int full_read(int fd,void *buffer,size_t length,off_t offset) {
    unsigned char *p=buffer;size_t total=0;
    while (total<length) {
        ssize_t n=pread(fd,p+total,length-total,offset+(off_t)total);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) return -1;
        total+=(size_t)n;
    }
    return 0;
}
static int available_sources(const tracker_t *t,uint64_t start,uint64_t end,unsigned indices[MAX_PEERS]) {
    unsigned count=0;
    for (unsigned i=0;i<t->peer_count;i++) {
        if (t->peers[i].start <= start && t->peers[i].end >= end) {
            unsigned j=count++;
            while (j>0 && t->peers[indices[j-1]].timestamp < t->peers[i].timestamp) {
                indices[j]=indices[j-1];j--;
            }
            indices[j]=i;
        }
    }
    return (int)count;
}
static int get_chunk(const download_chunk_t *chunk,unsigned attempt,void *arg) {
    transfer_ctx_t *ctx=arg;
    const tracker_t *t=ctx->tracker;
    uint64_t end=chunk->offset+chunk->length;
    unsigned indices[MAX_PEERS];
    int n=available_sources(t,chunk->offset,end,indices);
    if (n==0) return -1;
    const source_peer_t *p=&t->peers[indices[(attempt-1)%(unsigned)n]];
    int fd=net_connect_tcp(p->host,p->port,4000);
    if (fd<0) return -1;
    char request[256],reply[64];
    int length=snprintf(request,sizeof request,"<GET %s %" PRIu64 " %u>\n",
                        t->filename,chunk->offset,(unsigned)chunk->length);
    int ok=length>0 && (size_t)length<sizeof request &&
        net_send_all(fd,request,(size_t)length)==0 &&
        net_recv_line(fd,reply,sizeof reply)>0;
    unsigned long got=0;char extra=0;
    if (!ok || sscanf(reply,"<DATA %lu> %c",&got,&extra)!=1 ||
        got!=chunk->length) { close(fd);return -1; }
    unsigned char data[1024];
    ok=net_recv_exact(fd,data,chunk->length)==0;
    close(fd);
    if (!ok) return -1;
    if (full_write(ctx->file_fd,data,chunk->length,(off_t)chunk->offset)!=0) return -1;
    /* Per-chunk one-byte completion log supports best-effort restart recovery.
     * A power failure may require a fresh integrity-checked download. */
    const char complete='1';
    if (full_write(ctx->state_fd,&complete,1,(off_t)STATE_HEADER+(off_t)(chunk->offset/1024))!=0)
        return -1;
    pthread_mutex_lock(&ctx->log_mutex);
    ctx->completed++;
    fprintf(stdout,"[download] chunk %" PRIu64 " (%u bytes) from %s:%u complete\n",
        chunk->offset,chunk->length,p->host,(unsigned)p->port);
    fflush(stdout);
    pthread_mutex_unlock(&ctx->log_mutex);
    return 0;
}

int node_download_from_cache(const char *cache_file,const char *shared_dir) {
    tracker_t *t=calloc(1,sizeof *t);
    if (!t) return -1;
    if (read_tracker(cache_file,t)<0) { free(t);return -1; }
    char output[4096],part[4096],state[4096];
    int len=snprintf(output,sizeof output,"%s/%s",shared_dir,t->filename);
    if (len<0 || (size_t)len>=sizeof output) {free(t);return -1;}
    len=snprintf(part,sizeof part,"%s.part",output);
    if (len<0||(size_t)len>=sizeof part) {free(t);return -1;}
    len=snprintf(state,sizeof state,"%s.state",part);
    if (len<0||(size_t)len>=sizeof state) {free(t);return -1;}
    char actual[33];
    if (file_md5(output,actual)==0 && strcasecmp(actual,t->md5)==0) {
        printf("[download] Already complete: %s\n",output);
        unlink(cache_file);free(t);return 0;
    }
    size_t count=(size_t)(t->filesize/1024+(t->filesize%1024!=0));
    if (count>MAX_CHUNKS) {fprintf(stderr,"[download] File too large for current chunk map\n");free(t);return -1;}
    char header[STATE_HEADER]={0};
    snprintf(header,sizeof header,"CHUNKSTATE1 %" PRIu64 " %s\n",t->filesize,t->md5);
    int partfd=open(part,O_CREAT|O_RDWR,0600);
    if (partfd<0) {perror(part);free(t);return -1;}
    int statefd=open(state,O_CREAT|O_RDWR,0600);
    if (statefd<0) {perror(state);close(partfd);free(t);return -1;}
    char existing[STATE_HEADER];struct stat partstat,statestat;
    int resume=fstat(partfd,&partstat)==0 && fstat(statefd,&statestat)==0 &&
       partstat.st_size==(off_t)t->filesize &&
       statestat.st_size==(off_t)(STATE_HEADER+count) &&
       full_read(statefd,existing,sizeof existing,0)==0 &&
       !memcmp(header,existing,sizeof existing);
    if (!resume) {
        if (ftruncate(partfd,(off_t)t->filesize)<0 ||
            ftruncate(statefd,(off_t)(STATE_HEADER+count))<0 ||
            full_write(statefd,header,sizeof header,0)<0) {
            perror("[download] initialize partial files");
            close(partfd);close(statefd);free(t);return -1;
        }
        char empty[1024]={0};
        for (size_t i=0;i<count;i+=sizeof empty) {
            size_t n=count-i<sizeof empty?count-i:sizeof empty;
            if (full_write(statefd,empty,n,STATE_HEADER+(off_t)i)<0) {
                close(partfd);close(statefd);free(t);return -1;
            }
        }
    }
    unsigned char *bitmap=calloc(count?count:1,1);
    download_chunk_t *jobs=calloc(count?count:1,sizeof *jobs);
    unsigned char *status=calloc(count?count:1,1);
    if (!bitmap||!jobs||!status) {
        free(bitmap);free(jobs);free(status);close(partfd);close(statefd);free(t);return -1;
    }
    if (count && full_read(statefd,bitmap,count,STATE_HEADER)<0) {
        free(bitmap);free(jobs);free(status);close(partfd);close(statefd);free(t);return -1;
    }
    size_t pending=0,already=0;
    for (size_t i=0;i<count;i++) {
        if (bitmap[i]=='1') {already++;continue;}
        uint64_t offset=(uint64_t)i*1024;
        unsigned length=(unsigned)((t->filesize-offset)>1024?1024:(t->filesize-offset));
        unsigned candidates[MAX_PEERS];
        if (!available_sources(t,offset,offset+length,candidates)) {
            fprintf(stderr,"[download] No source covers bytes %" PRIu64 "..%" PRIu64 "\n",offset,offset+length);
            free(bitmap);free(jobs);free(status);close(partfd);close(statefd);free(t);return -1;
        }
        jobs[pending].offset=offset;
        jobs[pending++].length=(uint16_t)length;
    }
    printf("[download] Starting %s (%" PRIu64 " bytes, %zu peers, %zu chunks, %zu resumed)\n",
        t->filename,t->filesize,(size_t)t->peer_count,count,already);
    transfer_ctx_t ctx={.tracker=t,.file_fd=partfd,.state_fd=statefd};
    pthread_mutex_init(&ctx.log_mutex,NULL);
    int rc=download_pool_run(jobs,pending,4,3,get_chunk,&ctx,status);
    pthread_mutex_destroy(&ctx.log_mutex);
    free(bitmap);free(jobs);free(status);
    if (rc!=0) {
        fprintf(stderr,"[download] Incomplete download; .part and .state retained for retry\n");
        close(partfd);close(statefd);free(t);return -1;
    }
    if (fsync(partfd)!=0 || fsync(statefd)!=0) {
        perror("[download] sync");close(partfd);close(statefd);free(t);return -1;
    }
    close(partfd);close(statefd);
    if (file_md5(part,actual)<0 || strcasecmp(actual,t->md5)!=0) {
        fprintf(stderr,"[download] File MD5 mismatch; clearing chunk state for clean retry\n");
        unlink(state);free(t);return -1;
    }
    if (rename(part,output)!=0) {perror("[download] rename");free(t);return -1;}
    unlink(state);
    if (unlink(cache_file)!=0) perror("[download] cache cleanup");
    printf("[download] COMPLETE %s (MD5 %s)\n",output,actual);
    free(t);return 0;
}
