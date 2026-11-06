/* Member 4 integration layer: preserve the existing Member 2 peer.c unchanged.
 * A single interactive 'node' starts Member 3's chunk-serving handler via
 * Member 4's pthread framework, invokes peer.c's tested tracker CLI as a
 * child process, and automatically downloads verified GET metadata using
 * Member 4's four-worker chunk pool. No shell interpretation is involved.
 */
#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "net_io.h"
#include "node_download.h"
#include "peer_server.h"
#include "thread_server.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested=0;
static volatile sig_atomic_t server_failed=0;
static void stop_handler(int signum) { (void)signum;stop_requested=1; }
typedef struct {
    thread_server_options_t opts;
    server_config_t server_cfg;
} server_t;

static void *server_main(void *arg) {
    server_t *s=arg;
    if (thread_server_run(&s->opts,peer_serve_connection,s->server_cfg.shared_dir,&stop_requested)<0) {
        perror("[node] peer server");server_failed=1;
    }
    return NULL;
}
static int safe_filename(const char *s) {
    size_t n=strlen(s);
    if (!n||n>180||s[0]=='.') return 0;
    for (const unsigned char *p=(const unsigned char *)s;*p;p++)
        if (!isalnum(*p)&&*p!='-'&&*p!='_'&&*p!='.') return 0;
    return 1;
}
static int run_client(char *const command[]) {
    char *args[16]={"./peer"};
    unsigned i=1;
    for (unsigned j=0;command[j];j++) {
        if (i>=15) {fprintf(stderr,"[node] Too many command fields\n");return -1;}
        args[i++]=command[j];
    }
    args[i]=NULL;
    fflush(stdout);
    pid_t pid=fork();
    if (pid<0) {perror("fork");return -1;}
    if (pid==0) {
        execv("./peer",args);
        perror("[node] exec peer");_exit(127);
    }
    int status=0;
    while (waitpid(pid,&status,0)<0) {
        if (errno==EINTR) continue;
        perror("waitpid");return -1;
    }
    return WIFEXITED(status)&&WEXITSTATUS(status)==0?0:-1;
}
static int guess_local_ip(const client_config_t *config,char out[INET_ADDRSTRLEN]) {
    int fd=net_connect_tcp(config->tracker_ip,config->tracker_port,2500);
    if (fd<0) return -1;
    struct sockaddr_in addr; socklen_t n=sizeof addr;
    int ok=getsockname(fd,(struct sockaddr *)&addr,&n)==0 &&
           addr.sin_family==AF_INET &&
           inet_ntop(AF_INET,&addr.sin_addr,out,INET_ADDRSTRLEN)!=NULL;
    close(fd);return ok?0:-1;
}
static int get_file_size_md5(const char *path,uint64_t *size,char md5[33]) {
    struct stat st;
    if (stat(path,&st)<0 || !S_ISREG(st.st_mode) || st.st_size<0) return -1;
    FILE *f=fopen(path,"rb");if (!f) return -1;
    EVP_MD_CTX *ctx=EVP_MD_CTX_new();
    int ok=ctx && EVP_DigestInit_ex(ctx,EVP_md5(),NULL);
    unsigned char buf[16384];size_t n;
    while (ok && (n=fread(buf,1,sizeof buf,f))>0)
        ok=EVP_DigestUpdate(ctx,buf,n);
    if (ferror(f)) ok=0;
    unsigned char digest[EVP_MAX_MD_SIZE];unsigned int len=0;
    if (ok) ok=EVP_DigestFinal_ex(ctx,digest,&len)&&len==16;
    EVP_MD_CTX_free(ctx);fclose(f);
    if (!ok) return -1;
    *size=(uint64_t)st.st_size;
    for (unsigned i=0;i<16;i++) snprintf(md5+2*i,3,"%02x",digest[i]);
    return 0;
}
static int report_completed(const char *filename,const server_t *s,const client_config_t *client) {
    char host[INET_ADDRSTRLEN];
    if (guess_local_ip(client,host)<0) {
        fprintf(stderr,"[node] File complete; cannot determine advertised IP for tracker update\n");
        return -1;
    }
    char path[4096],size_str[32],port_str[6];
    if (snprintf(path,sizeof path,"%s/%s",s->server_cfg.shared_dir,filename)>=(int)sizeof path)
        return -1;
    uint64_t size;char digest[33];
    if (get_file_size_md5(path,&size,digest)<0) return -1;
    snprintf(size_str,sizeof size_str,"%" PRIu64,size);
    snprintf(port_str,sizeof port_str,"%u",(unsigned)s->server_cfg.listen_port);
    char *args[]={"updatetracker",(char *)filename,"0",size_str,host,port_str,NULL};
    return run_client(args);
}
static int execute_command(char *const args[],server_t *server,const client_config_t *client) {
    if (!args[0]) return 0;
    if (!strcasecmp(args[0],"GET")&&args[1]&&!args[2]) {
        if (run_client(args)!=0) return -1;
        char cache[4096];
        if (snprintf(cache,sizeof cache,"cache/%s",args[1]) >= (int)sizeof cache) return -1;
        if (node_download_from_cache(cache,server->server_cfg.shared_dir)<0) return -1;
        size_t n=strlen(args[1]);
        if (n>6 && !strcmp(args[1]+n-6,".track")) {
            char filename[181];
            if (n-6>=sizeof filename) return -1;
            memcpy(filename,args[1],n-6);filename[n-6]=0;
            if (report_completed(filename,server,client)<0)
                fprintf(stderr,"[node] Warning: file succeeded but tracker update did not\n");
        }
        return 0;
    }
    if (!strcasecmp(args[0],"RETRY")&&args[1]&&!args[2]&&safe_filename(args[1])) {
        char path[4096];
        if (snprintf(path,sizeof path,"cache/%s",args[1])>=(int)sizeof path) return -1;
        if (node_download_from_cache(path,server->server_cfg.shared_dir)<0) return -1;
        size_t n=strlen(args[1]);
        if (n>6 && !strcmp(args[1]+n-6,".track")) {
            char filename[181];
            if (n-6>=sizeof filename) return -1;
            memcpy(filename,args[1],n-6);filename[n-6]=0;
            if (report_completed(filename,server,client)<0)
                fprintf(stderr,"[node] Warning: file completed but tracker update did not\n");
        }
        return 0;
    }
    if (!strcasecmp(args[0],"SEED")&&args[1]&&args[2]&&!args[3]&&
        safe_filename(args[1])&&!strpbrk(args[2]," <>\t\n")) {
        char path[4096];
        if (snprintf(path,sizeof path,"%s/%s",server->server_cfg.shared_dir,args[1]) >= (int)sizeof path)
            return -1;
        uint64_t size;char md5[33],host[INET_ADDRSTRLEN],size_str[32],port_str[6];
        if (get_file_size_md5(path,&size,md5)<0 || guess_local_ip(client,host)<0) {
            perror("[node] seed file or tracker IP");return -1;
        }
        snprintf(size_str,sizeof size_str,"%" PRIu64,size);
        snprintf(port_str,sizeof port_str,"%u",(unsigned)server->server_cfg.listen_port);
        char *cmd[]={"createtracker",args[1],size_str,args[2],md5,host,port_str,NULL};
        return run_client(cmd);
    }
    return run_client(args);
}
static int command_tokens(char *line,char **fields,unsigned limit) {
    unsigned n=0;char *save=NULL;
    for (char *p=strtok_r(line," \t\r\n",&save);p;p=strtok_r(NULL," \t\r\n",&save)) {
        if (n>=limit-1) return -1;
        fields[n++]=p;
    }
    fields[n]=NULL;
    return (int)n;
}
int main(int argc,char **argv) {
    signal(SIGPIPE,SIG_IGN);
    signal(SIGTERM,stop_handler);
    signal(SIGINT,stop_handler);
    server_t server={0};client_config_t client;
    if (config_read_server("serverThreadConfig.cfg",&server.server_cfg)!=0 ||
        config_read_client("clientThreadConfig.cfg",&client)!=0) {
        perror("[node] configuration");return 1;
    }
    if (mkdir(server.server_cfg.shared_dir,0700)<0 && errno!=EEXIST) {
        perror("[node] shared directory");return 1;
    }
    server.opts=(thread_server_options_t){.bind_ip="0.0.0.0",
        .port=server.server_cfg.listen_port,.max_clients=64,
        .io_timeout_ms=5000,.backlog=64};
    pthread_t server_thread;
    int rc=pthread_create(&server_thread,NULL,server_main,&server);
    if (rc!=0) {errno=rc;perror("[node] thread startup");return 1;}
    printf("[node] Peer server starting at 0.0.0.0:%u (shared: %s), tracker %s:%u\n",
        (unsigned)server.server_cfg.listen_port,server.server_cfg.shared_dir,
        client.tracker_ip,(unsigned)client.tracker_port);
    fflush(stdout);
    /* Detect server startup failures before issuing any tracker commands. */
    struct timespec wait_time={0,200000000L};nanosleep(&wait_time,NULL);
    if (server_failed) {stop_requested=1;pthread_join(server_thread,NULL);return 1;}
    int ok=1;
    if (argc>1) {
        if (execute_command(argv+1,&server,&client)<0) ok=0;
    } else {
        puts("[node] Commands: LIST, GET file.track, SEED filename description, ");
        puts("[node]           createtracker ..., updatetracker ..., RETRY file.track, quit");
        char line[2048];
        while (!stop_requested) {
            printf("node> ");fflush(stdout);
            if (!fgets(line,sizeof line,stdin)) break;
            if (!strchr(line,'\n') && !feof(stdin)) {
                int c;while ((c=getchar())!='\n'&&c!=EOF) {};
                fprintf(stderr,"[node] command too long\n");ok=0;continue;
            }
            char *fields[16]={0};int n=command_tokens(line,fields,16);
            if (n<0) {fprintf(stderr,"[node] too many fields\n");ok=0;continue;}
            if (!n) continue;
            if (!strcmp(fields[0],"quit")||!strcmp(fields[0],"exit")) break;
            if (execute_command(fields,&server,&client)<0) ok=0;
        }
    }
    stop_requested=1;
    pthread_join(server_thread,NULL);
    return (ok && !server_failed)?0:1;
}
