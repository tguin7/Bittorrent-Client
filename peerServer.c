/* Member 3's file-serving GET implementation, adapted for integration.
 * Supports the original Member 3 wire format and the team's framed variant:
 *   GET filename offset length\n      -> exactly 'length' raw bytes
 *   <GET filename offset length>\n    -> <DATA length>\n + raw bytes
 * On errors both variants return <GET invalid>\n or <GET missing>\n.
 * One request per connection, maximum chunk length 1024 bytes.
 * Member 4's thread_server dispatches concurrent connections and owns fd.
 */
#define _POSIX_C_SOURCE 200809L
#include "peer_server.h"
#include "net_io.h"
#include "thread_server.h"
#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static int safe_filename(const char *name) {
    size_t n = strlen(name);
    if (!n || n > 180 || name[0] == '.') return 0;
    for (const unsigned char *p=(const unsigned char *)name; *p; ++p)
        if (!isalnum(*p) && *p != '_' && *p != '-' && *p != '.') return 0;
    return 1;
}

static int decimal(const char *s, uint64_t *out) {
    if (!s || !*s) return 0;
    uint64_t val=0;
    for (; *s; ++s) {
        if (!isdigit((unsigned char)*s)) return 0;
        unsigned digit=(unsigned)(*s-'0');
        if (val > (UINT64_MAX-digit)/10) return 0;
        val=10*val+digit;
    }
    *out=val;
    return 1;
}

static void send_error(int fd, const char *text) {
    (void)net_send_all(fd,text,strlen(text));
}

void peer_serve_connection(int fd, void *shared_dir_context) {
    const char *shared_dir=(const char *)shared_dir_context;
    if (!shared_dir || !*shared_dir) return;
    char request[512];
    ssize_t size=net_recv_line(fd,request,sizeof request);
    if (size < 0) {
        send_error(fd,"<GET invalid>\n");
        return;
    }
    if (request[size-1] != '\n') { send_error(fd,"<GET invalid>\n"); return; }
    request[size-1]='\0';
    int framed=request[0]=='<';
    char *body=request;
    if (framed) {
        size_t end=strlen(request);
        if (end < 2 || request[end-1] != '>') {
            send_error(fd,"<GET invalid>\n"); return;
        }
        request[end-1]='\0';
        body=request+1;
    }
    char *parts[5]={0};
    char *save=NULL;
    unsigned count=0;
    for (char *p=strtok_r(body," \t\r",&save); p; p=strtok_r(NULL," \t\r",&save)) {
        if (count >= 4) { count=5; break; }
        parts[count++]=p;
    }
    uint64_t offset=0, length=0;
    if (count != 4 || strcmp(parts[0],"GET") || !safe_filename(parts[1]) ||
        !decimal(parts[2],&offset) || !decimal(parts[3],&length) ||
        length == 0 || length > 1024 || offset > INT64_MAX ||
        offset > UINT64_MAX-length) {
        send_error(fd,"<GET invalid>\n"); return;
    }
    char path[4096];
    int n=snprintf(path,sizeof path,"%s/%s",shared_dir,parts[1]);
    if (n < 0 || (size_t)n >= sizeof path) { send_error(fd,"<GET invalid>\n"); return; }
    FILE *file=fopen(path,"rb");
    if (!file) { send_error(fd,"<GET missing>\n"); return; }
    if (fseeko(file,(off_t)offset,SEEK_SET) != 0) {
        fclose(file); send_error(fd,"<GET invalid>\n"); return;
    }
    unsigned char chunk[1024];
    size_t got=fread(chunk,1,(size_t)length,file);
    int read_error=ferror(file);
    fclose(file);
    if (read_error || got != (size_t)length) {
        send_error(fd,"<GET invalid>\n"); return;
    }
    if (framed) {
        char header[32];
        int h=snprintf(header,sizeof header,"<DATA %zu>\n",got);
        if (h <= 0 || net_send_all(fd,header,(size_t)h) != 0) return;
    }
    if (net_send_all(fd,chunk,got) == 0) {
        fprintf(stdout,"[peerServer] Served %s offset=%" PRIu64 " length=%zu mode=%s\n",
                parts[1],offset,got,framed?"framed":"legacy");
        fflush(stdout);
    }
}

#ifdef PEER_SERVER_STANDALONE
static volatile sig_atomic_t stop_requested=0;
static void stop_handler(int sig) { (void)sig; stop_requested=1; }
int main(int argc, char **argv) {
    const char *cfg=argc>=2?argv[1]:"serverThreadConfig.cfg";
    server_config_t c;
    if (config_read_server(cfg,&c) != 0) { perror(cfg); return 1; }
    signal(SIGINT,stop_handler);
    signal(SIGTERM,stop_handler);
    thread_server_options_t opts={.bind_ip="0.0.0.0",.port=c.listen_port,
        .max_clients=64,.io_timeout_ms=5000,.backlog=64};
    printf("Peer server listening on port %u, shared directory %s\n",
            (unsigned)c.listen_port,c.shared_dir);
    fflush(stdout);
    if (thread_server_run(&opts,peer_serve_connection,c.shared_dir,&stop_requested)<0) {
        perror("peer server");return 1;
    }
    return 0;
}
#endif
