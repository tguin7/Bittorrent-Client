/* CS4390 peer client: tracker communication. Based on the supplied peer.c
 * socket/connect/request outline; undefined numeric LIST replaced by PDF text.
 * POSIX C11. OpenSSL EVP supplies MD5; no downloaded source is embedded.
 */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <openssl/evp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LIMIT (1024 * 1024)
#define TIMEOUT_MS 5000
typedef struct { char host[256], port[6]; unsigned long interval; } Config;
static void error(const char *s) { fprintf(stderr, "Error: %s\n", s); }
static int number(const char *s, unsigned long long *n) {
    char *end;
    if (!*s) return 0;
    for (const char *p=s; *p; ++p) if (!isdigit((unsigned char)*p)) return 0;
    errno=0; *n=strtoull(s,&end,10);
    return !errno && !*end;
}
static int filename(const char *s) {
    size_t n=strlen(s);
    if (!n || n>180 || s[0]=='.') return 0;
    for (; *s; ++s) if (!isalnum((unsigned char)*s) && !strchr("._-",*s)) return 0;
    return 1;
}
static int digest_text(const char *s) {
    if (strlen(s)!=32) return 0;
    for (; *s; ++s) if (!isxdigit((unsigned char)*s)) return 0;
    return 1;
}
static int md5(const void *data, size_t len, char out[33]) {
    unsigned char bytes[EVP_MAX_MD_SIZE]; unsigned int n=0;
    if (!EVP_Digest(data,len,bytes,&n,EVP_md5(),NULL) || n!=16) return 0;
    for (unsigned int i=0;i<n;++i) snprintf(out+2*i,3,"%02x",bytes[i]);
    return 1;
}
static int config_read(const char *path, Config *c) {
    FILE *f=fopen(path,"r"); char line[300]; unsigned long long val;
    if (!f) { perror(path); return 0; }
    for (int i=0;i<3;++i) {
        if (!fgets(line,sizeof line,f)) goto bad;
        line[strcspn(line,"\r\n")]=0;
        if (i==0) {
            if (!number(line,&val)||val<1||val>65535) goto bad;
            snprintf(c->port,sizeof c->port,"%llu",val);
        } else if (i==1) {
            unsigned char addr[16];
            if (inet_pton(AF_INET,line,addr)!=1 && inet_pton(AF_INET6,line,addr)!=1) goto bad;
            strcpy(c->host,line);
        } else {
            if (!number(line,&val)||!val||val>ULONG_MAX) goto bad;
            c->interval=(unsigned long)val;
        }
    }
    while (fgets(line,sizeof line,f)) for (char *p=line;*p;++p) if (!isspace((unsigned char)*p)) goto bad;
    if (ferror(f)) goto bad;
    fclose(f); return 1;
bad:
    fclose(f); error("config must contain port, numeric tracker IP, refresh seconds on three lines"); return 0;
}
static long long now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (long long)t.tv_sec*1000+t.tv_nsec/1000000;
}
/* One absolute deadline prevents a slow sender from keeping us alive forever. */
static int ready(int fd, short events, long long deadline) {
    struct pollfd p={fd,events,0}; int rc;
    for (;;) {
        long long left=deadline-now_ms();
        if (left<=0) { error("network deadline exceeded"); return 0; }
        rc=poll(&p,1,(int)left);
        if (rc<0 && errno==EINTR) continue;
        if (rc<=0) { error("network timeout or poll error"); return 0; }
        return 1; /* recv/send/SO_ERROR reports hangup and socket errors */
    }
}
static char *exchange(const Config *c,const char *request,size_t *size) {
    struct addrinfo hints={0},*ai=NULL; int fd=-1; char *buf=NULL;
    long long deadline=now_ms()+TIMEOUT_MS; size_t sent=0,used=0;
    hints.ai_socktype=SOCK_STREAM; hints.ai_flags=AI_NUMERICHOST|AI_NUMERICSERV;
    if (getaddrinfo(c->host,c->port,&hints,&ai)) { error("invalid tracker address"); return NULL; }
    fd=socket(ai->ai_family,ai->ai_socktype,ai->ai_protocol);
    if (fd<0) goto fail;
    if (fcntl(fd,F_SETFL,O_NONBLOCK)<0) goto fail;
    if (connect(fd,ai->ai_addr,ai->ai_addrlen)<0) {
        int e=0; socklen_t len=sizeof e;
        if (errno!=EINPROGRESS || !ready(fd,POLLOUT,deadline)) goto fail;
        if (getsockopt(fd,SOL_SOCKET,SO_ERROR,&e,&len)<0 || e) { if(e) errno=e; goto fail; }
    }
    printf("Connected to tracker %s:%s\nSending: %s",c->host,c->port,request);
    while (sent<strlen(request)) {
        if (!ready(fd,POLLOUT,deadline)) goto fail;
        ssize_t n=send(fd,request+sent,strlen(request)-sent,0);
        if (n<0 && (errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK)) continue;
        if(n<=0) goto fail;
        sent+=(size_t)n;
    }
    buf=malloc(LIMIT+1);
    if(!buf) goto fail;
    /* The specification requires the tracker worker to close after its reply. */
    for (;;) {
        char extra;
        if(!ready(fd,POLLIN,deadline)) goto fail;
        ssize_t n=recv(fd,used<LIMIT ? buf+used : &extra,used<LIMIT ? LIMIT-used : 1,0);
        if(n<0 && (errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK)) continue;
        if(n<0) goto fail;
        if(!n) break;
        if(used==LIMIT) { error("response exceeds 1 MiB"); goto fail; }
        used+=(size_t)n;
    }
    close(fd); freeaddrinfo(ai);
    if(memchr(buf,0,used)) { error("NUL in text response"); free(buf); return NULL; }
    buf[used]=0; *size=used; return buf;
fail:
    perror("tracker communication");
    if(fd>=0) close(fd);
    freeaddrinfo(ai); free(buf); return NULL;
}
/* Split tokens only after framing has been checked. */
static int tokens(char *s,char **v,int max) {
    int n=0; char *save=NULL,*p=strtok_r(s," \t\r\n",&save);
    while(p) { if(n==max) return -1; v[n++]=p; p=strtok_r(NULL," \t\r\n",&save); }
    return n;
}
static char *line_next(char **cursor) {
    char *s=*cursor,*end=strchr(s,'\n');
    if(!end) return NULL;
    *end=0; *cursor=end+1; return s;
}
static int frame(char *s) {
    size_t n=strlen(s);
    if(n<2||s[0]!='<'||s[n-1]!='>') return 0;
    memmove(s,s+1,n-2); s[n-2]=0; return 1;
}
static int list_parse(char *reply) {
    char *cursor=reply,*line,*v[5]; unsigned long long count,index,size;
    line=line_next(&cursor);
    if(!line||!frame(line)||tokens(line,v,5)!=3||strcmp(v[0],"REP")||strcmp(v[1],"LIST")||!number(v[2],&count)||count>10000) return 0;
    for(unsigned long long i=1;i<=count;++i) {
        line=line_next(&cursor);
        if(!line||!frame(line)||tokens(line,v,5)!=4||!number(v[0],&index)||index!=i||!filename(v[1])||!number(v[2],&size)||!digest_text(v[3])) return 0;
        printf("%llu. %s | %llu bytes | MD5 %s\n",i,v[1],size,v[3]);
    }
    if(strcmp(cursor,"<REP LIST END>\n")) return 0;
    printf("LIST complete: %llu file(s).\n",count); return 1;
}
/* Validate metadata separately from hashing; never normalize bytes before MD5. */
static int metadata(const char *body,size_t len,const char *requested) {
    char *copy=malloc(len+1),*cursor,*line; int fields=0,peers=0,ok=0;
    unsigned long long filesize=0;
    if(!copy) return 0;
    memcpy(copy,body,len); copy[len]=0; cursor=copy;
    while(*cursor) {
        line=cursor; char *end=strchr(cursor,'\n');
        if(end) { *end=0; cursor=end+1; } else cursor+=strlen(cursor);
        size_t l=strlen(line); if(l && line[l-1]=='\r') line[l-1]=0;
        if(!*line||*line=='#') continue;
        if(!strncmp(line,"Filename: ",10)) {
            char expected[200]; snprintf(expected,sizeof expected,"%s.track",line+10);
            if(fields||!filename(line+10)||strcmp(expected,requested)) goto done;
            fields=1;
        } else if(!strncmp(line,"Filesize: ",10)) {
            if(fields!=1||!number(line+10,&filesize)) goto done;
            fields=2;
        } else if(!strncmp(line,"Description:",12)) {
            if(fields!=2) goto done;
            fields=3;
        } else if(!strncmp(line,"MD5: ",5)) {
            if((fields!=2&&fields!=3)||!digest_text(line+5)) goto done;
            fields=4;
        } else {
            char *v[6],*p=line; unsigned long long port,start,finish,stamp; struct in_addr ip;
            if(fields!=4) goto done;
            /* IPv4 colon-separated peer records are the assignment format. */
            for(int i=0;i<5;++i) {
                v[i]=p; char *sep=strchr(p,':');
                if(i<4) { if(!sep) goto done; *sep=0; p=sep+1; }
                else if(sep) goto done;
            }
            if(inet_pton(AF_INET,v[0],&ip)!=1||!number(v[1],&port)||port<1||port>65535||!number(v[2],&start)||!number(v[3],&finish)||start>finish||finish>filesize||!number(v[4],&stamp)) goto done;
            ++peers;
        }
    }
    ok=fields==4;
    if(ok) printf("Tracker parsed: %llu shared-file bytes, %d peer record(s).\n",filesize,peers);
done:
    free(copy); return ok;
}
static int cache_save(const char *directory,const char *name,const char *body,size_t len) {
    char path[PATH_MAX],temp[PATH_MAX]; int fd; FILE *f;
    if(mkdir(directory,0700)<0 && errno!=EEXIST) { perror(directory); return 0; }
    if(snprintf(path,sizeof path,"%s/%s",directory,name)>=(int)sizeof path || snprintf(temp,sizeof temp,"%s/.incoming-XXXXXX",directory)>=(int)sizeof temp) return 0;
    fd=mkstemp(temp); if(fd<0) { perror("cache temporary file"); return 0; }
    f=fdopen(fd,"wb"); if(!f) { close(fd); unlink(temp); return 0; }
    int ok=fwrite(body,1,len,f)==len;
    if(fflush(f)||fsync(fd)) ok=0;
    if(fclose(f)) ok=0;
    if(ok && rename(temp,path)==0) { printf("MD5 verified. Cached tracker: %s\n",path); return 1; }
    perror("cache write"); unlink(temp); return 0;
}
static int get_parse(char *reply,size_t len,const char *name,const char *cache) {
    const char *begin="<REP GET BEGIN>\n",*marker="<REP GET END ";
    size_t head=strlen(begin),tail= strlen(marker)+32+2;
    char expected[33],actual[33];
    if(len<head+tail||memcmp(reply,begin,head)) return 0;
    char *footer=reply+len-tail;
    if(memcmp(footer,marker,strlen(marker))||memcmp(reply+len-2,">\n",2)) return 0;
    memcpy(expected,footer+strlen(marker),32); expected[32]=0;
    if(!digest_text(expected)) return 0;
    char *body=reply+head; size_t n=(size_t)(footer-body);
    /* PDF uses <tracker_file_content > as a placeholder. Accept raw bytes or
     * a literal single wrapper, only when the chosen exact bytes match MD5. */
    if(!md5(body,n,actual)) return 0;
    if(strcasecmp(expected,actual)) {
        if(n<4||body[0]!='<'||memcmp(body+n-3," >\n",3)) { error("MD5 mismatch"); return 0; }
        ++body; n-=4;
        if(!md5(body,n,actual)||strcasecmp(expected,actual)) { error("MD5 mismatch"); return 0; }
    }
    if(!metadata(body,n,name)) { error("invalid tracker metadata"); return 0; }
    if(!cache_save(cache,name,body,n)) return 0;
    puts("Tracker retrieval complete."); return 1;
}
static int run(const Config *c,const char *cache,char *command) {
    char *v[10],request[4096]; int n=tokens(command,v,10),kind=0;
    unsigned long long a,b,port; struct in_addr ip;
    if(n==1&&!strcasecmp(v[0],"LIST")) kind=1;
    if(n==2&&!strcasecmp(v[0],"REQ")&&!strcasecmp(v[1],"LIST")) kind=1;
    if(kind==1) strcpy(request,"<REQ LIST>\n");
    else if(n==2&&!strcasecmp(v[0],"GET")&&filename(v[1])&&strlen(v[1])>6&&!strcmp(v[1]+strlen(v[1])-6,".track")) {
        kind=2; snprintf(request,sizeof request,"<GET %s >\n",v[1]);
    } else if(n==7&&!strcasecmp(v[0],"createtracker")&&filename(v[1])&&number(v[2],&a)&&digest_text(v[4])&&inet_pton(AF_INET,v[5],&ip)==1&&number(v[6],&port)&&port>0&&port<=65535&&!strpbrk(v[3],"<>")) {
        kind=3; snprintf(request,sizeof request,"<createtracker %s %s %s %s %s %s>\n",v[1],v[2],v[3],v[4],v[5],v[6]);
    } else if(n==6&&!strcasecmp(v[0],"updatetracker")&&filename(v[1])&&number(v[2],&a)&&number(v[3],&b)&&a<=b&&inet_pton(AF_INET,v[4],&ip)==1&&number(v[5],&port)&&port>0&&port<=65535) {
        kind=4; snprintf(request,sizeof request,"<updatetracker %s %s %s %s %s>\n",v[1],v[2],v[3],v[4],v[5]);
    } else { error("invalid command; use LIST, GET name.track, createtracker or updatetracker (see README)"); return 0; }
    size_t len; char *reply=exchange(c,request,&len); int ok=0;
    if(!reply) return 0;
    if(kind==1) ok=list_parse(reply);
    else if(kind==2) ok=get_parse(reply,len,v[1],cache);
    else {
        char expected[256];
        if(kind==3) strcpy(expected,"<createtracker succ>\n");
        else snprintf(expected,sizeof expected,"<updatetracker %s succ>\n",v[1]);
        printf("Tracker response: %s",reply); ok=!strcmp(reply,expected);
    }
    if(!ok) error("tracker rejected request or reply failed validation; cache unchanged");
    free(reply); return ok;
}
int main(int argc,char **argv) {
    const char *cfg="clientThreadConfig.cfg",*cache="cache"; int i=1,ok=1; Config c;
    signal(SIGPIPE,SIG_IGN);
    while(i<argc&&argv[i][0]=='-') {
        if(!strcmp(argv[i],"--config")&&i+1<argc) { cfg=argv[i+1]; i+=2; }
        else if(!strcmp(argv[i],"--cache")&&i+1<argc) { cache=argv[i+1]; i+=2; }
        else { puts("Usage: peer [--config FILE] [--cache DIR] [COMMAND ...]"); return 2; }
    }
    if(!config_read(cfg,&c)) return 1;
    char command[2048];
    if(i<argc) {
        size_t used=0;
        for(;i<argc;++i) {
            size_t n=strlen(argv[i]);
            if(used+n+2>sizeof command) { error("command too long"); return 2; }
            memcpy(command+used,argv[i],n); used+=n; command[used++]=' ';
        }
        command[used]=0; return run(&c,cache,command)?0:1;
    }
    puts("Peer client. Enter LIST, GET filename.track, createtracker, updatetracker, or quit.");
    for(;;) {
        printf("peer> "); fflush(stdout);
        if(!fgets(command,sizeof command,stdin)) break;
        if(!strchr(command,'\n')&&!feof(stdin)) {
            int ch; while((ch=getchar())!='\n'&&ch!=EOF) {} error("command too long"); ok=0; continue;
        }
        command[strcspn(command,"\r\n")]=0;
        if(!strcmp(command,"quit")||!strcmp(command,"exit")) break;
        if(!*command) continue;
        if(!run(&c,cache,command)) ok=0;
    }
    return ok?0:1;
}
