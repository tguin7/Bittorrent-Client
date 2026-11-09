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

//check if filename is valid
static int safe_filename(const char *name)
 {
    size_t len = strlen(name);

    if(len == 0 || len > 180 || name[0] == '.') 
    {   
        return 0;
    }

    //check char in filename
    for(size_t i = 0; i < len; i++)
    {
        unsigned char ch = (unsigned char)name[i];

        if(!isalnum(ch) && ch != '_' && ch != '-' && ch != '.')
        {
            return 0;
        }
    }
    return 1;
}

//convert string of digits to int
static int decimal(const char *s, uint64_t *out) {
    //check for empty string
    if(s == NULL || *s == '\0')
    {
        return 0;
    } 

    uint64_t val=0;
    for(size_t i = 0; s[i] != '\0'; i++)
    {
        //check that char is digit
        if(!isdigit((unsigned char)s[i]))
        {
            return 0;
        } 
        unsigned digit=(unsigned)(s[i] - '0');

        //prevent from exceeding limit
        if(val > (UINT64_MAX - digit) / 10)
        {
            return 0;
        }

        //add digit to num
        val = (10 * val) + digit;
    }

    *out = val;
    return 1;
}

//error message
static void send_error(int fd, const char *message) 
{
    net_send_all(fd, message, strlen(message));
}

//GET request from  peer
void peer_serve_connection(int fd, void *dir_arg) 
{
    //get directory with files
    const char *shared_dir = (const char *)dir_arg;
    if(shared_dir == NULL || *shared_dir == '\0')
    {
        return;
    }

    char request[512];

    //rcv one line from peer
    ssize_t size = net_recv_line(fd, request, sizeof request);
    if(size <= 0) 
    {
        send_error(fd,"<GET invalid>\n");
        return;
    }

    if(request[size-1] != '\n') 
    { 
        send_error(fd,"<GET invalid>\n"); 
        return; 
    }

    request[size-1] = '\0';

    int framed = request[0] == '<';
    char *body = request;

    if (framed) 
    {
        size_t end = strlen(request);

        //Framed req end with >
        if (end < 2 || request[end-1] != '>')
        {
            send_error(fd,"<GET invalid>\n");
            return;
        }
        
        //Remove > and skip first <
        request[end-1]='\0';
        body=request+1;
    }

    //split req to 4
    char *parts[5] = {0};
    char *save = NULL;
    unsigned count = 0;

    char *token = strtok_r(body, " \t\r",&save);

    //split request until run out of tokens
    while(token != NULL)
    {
        if(count >= 4)
        {
            count = 5;
            break;
        }

        parts[count] = token;
        count++;

        token = strtok_r(NULL, " \t\r", &save);
    }

    uint64_t offset = 0;
    uint64_t length = 0;

    //Validate GET command and file chunk
    if(count != 4 || strcmp(parts[0],"GET") || !safe_filename(parts[1]) ||  !decimal(parts[2], &offset) || !decimal(parts[3], &length) ||
        length == 0 || length > 1024 || offset > INT64_MAX || offset > (UINT64_MAX - length)) 
    {
        send_error(fd, "<GET invalid>\n"); 
        return;
    }

    //Build file path
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/%s", shared_dir,parts[1]);

    //check that path fits in buffer
    if(n < 0 || (size_t)n >= sizeof(path))
    { 
        send_error(fd, "<GET invalid>\n"); 
        return;
    }

    //open file
    FILE *file = fopen(path, "rb");

    if(!file) 
    { 
        send_error(fd, "<GET missing>\n"); 
        return; 
    }

    //move to first byte
    if(fseeko(file, (off_t)offset, SEEK_SET) != 0) 
    {
        fclose(file);
        send_error(fd,"<GET invalid>\n"); 
        return;
    }

    //read chunk from file
    unsigned char chunk[1024];
    size_t bytes_read = fread(chunk, 1, (size_t)length, file);

    int read_err = ferror(file);

    fclose(file);

    if(read_err || bytes_read != (size_t)length)
    {
        send_error(fd, "<GET invalid>\n"); 
        return;
    }

    if(framed) 
    {
        char header[32];
        int h_len = snprintf(header, sizeof(header), "<DATA %zu>\n", bytes_read);

        if(h_len <= 0 || net_send_all(fd, header, (size_t)h_len) != 0) 
        {
            return;
        }
    }

    //send file bytes to peer
    if (net_send_all(fd, chunk, bytes_read) == 0) 
    {
        fprintf(stdout, "[peerServer] Served %s offset=%" PRIu64 " length=%zu mode=%s\n", parts[1],offset,bytes_read,framed? "framed" : "legacy");
        fflush(stdout);
    }
}

#ifdef PEER_SERVER_STANDALONE
static volatile sig_atomic_t stop_requested=0;

//shutdown
static void stop_handler(int sig) 
{
    (void)sig; 
    stop_requested = 1; 
}

int main(int argc, char **argv) 
{
    const char *cfg;

    if(argc >= 2)
    {
        cfg = argv[1];
    }
    else
    {
        cfg = "serverThreadConfig.cfg";
    }

    server_config_t config;

    //read server settings
    if(config_read_server(cfg, &config) != 0) 
    { 
        perror(cfg); 
        return 1; 
    }

    //Control C and termination signals
    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);

    //set up peer server options
    thread_server_options_t options = {.bind_ip="0.0.0.0", .port=config.listen_port, .max_clients=64, .io_timeout_ms = 5000, .backlog=64};

    printf("Peer server listening on port %u, shared directory %s\n", (unsigned)config.listen_port, config.shared_dir);
    fflush(stdout);

    //listen for other connections
    if (thread_server_run(&options, peer_serve_connection, config.shared_dir, &stop_requested) < 0) 
    {
        perror("peer server");
        return 1;
    }
    return 0;
}
#endif
