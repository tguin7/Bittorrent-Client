#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <dirent.h>
#include <openssl/evp.h>
#include "net_io.h"

#define BUFFER_SIZE 2048

int server_port = 0;
char shared_dir[256];
// Mutex to make sure there is thread-safe read/write operations on tracker files
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Wrapper for sending string replies over the TCP socket.
 * Uses the custom net_send_all function to ensure all bytes are transmitted.
 */
void send_reply(int socket, const char* msg) {
    (void)net_send_all(socket, msg, strlen(msg));
}

// Helper to remove brackets and newlines from manually typed commands
void clean_command(char* cmd) {
    if (cmd[0] == '<')
        memmove(cmd, cmd + 1, strlen(cmd));
    char *end = strchr(cmd, '>');
    if (end)
        *end = '\0';
    end = strchr(cmd, '\n');
    if (end)
        *end = '\0';
}

/* Replaces an existing peer record; don't append duplicates.
 * The tracker mutex is held by the caller throughout this transaction.
 * Preserve every other record and the exact metadata header.
 */
static int replace_peer_record(const char *path,const char *ip,int port,
                               long long first,long long last) {
    FILE *in=fopen(path,"r");
    if (!in)
        return -1;

    // Create a temporary file to safely write updates before overwriting the original
    char tmpname[1200];
    if (snprintf(tmpname,sizeof tmpname,"%s.tmp.XXXXXX",path)>=(int)sizeof tmpname) {
        fclose(in);return -1;
    }
    int fd=mkstemp(tmpname);
    if (fd<0) {
        fclose(in);
        return -1;
    }
    FILE *out=fdopen(fd,"w");
    if (!out) {
        close(fd);
        unlink(tmpname);
        fclose(in);
        return -1;
    }
    char *line=NULL;
    size_t cap=0;
    ssize_t n;
    long long filesize=-1;
    int ok=1;

    // Read the existing file line by line
    while ((n=getline(&line,&cap,in))!=-1) {
        // Extract the total file size from the metadata header
        if (!strncmp(line,"Filesize: ",10)) {
            long long x=-1;
            if (sscanf(line,"Filesize: %lld",&x)==1 && x>=0)
                filesize=x;
        }
        char oldip[64]={0};
        unsigned oldport=0;
        // If this line matches the IP and Port of the peer updating, skip writing it to the temp file
        if (sscanf(line,"%63[^:]:%u:",oldip,&oldport)==2 && strcmp(oldip,ip)==0 && oldport==(unsigned)port)
            continue;
        // Write all other lines to the temp file
        if (fwrite(line,1,(size_t)n,out)!=(size_t)n) {
            ok=0;
            break;
        }
    }
    if (ferror(in))
        ok=0;
    // Checks the wanted byte ranges against the known file size
    if (filesize<0 || first<0 || last<first || last>filesize)
        ok=0;
    // Append the newly updated peer record with a new system timestamp
    if (ok && fprintf(out,"%s:%d:%lld:%lld:%ld\n",ip,port,first,last,time(NULL))<0)
        ok=0;
    free(line);
    if (fclose(in)!=0)
        ok=0;
    // Ensure data is written to physical disk
    if (fflush(out)!=0 || fsync(fd)!=0)
        ok=0;
    if (fclose(out)!=0)
        ok=0;
    // Rename the temp file to the actual track file name
    if (ok && rename(tmpname,path)==0)
        return 0;
    unlink(tmpname);
    return -1;
}

/*
 * Worker thread function to handle individual peer connections.
 */
void* handle_client(void* arg) {
    int client_socket = *(int*)arg;
    free(arg);
    
    char buffer[BUFFER_SIZE];
    memset(buffer, 0, BUFFER_SIZE);
    
    // Receive a complete protocol command even when TCP fragments the line.
    ssize_t bytes_read = net_recv_line(client_socket, buffer, BUFFER_SIZE);
    if (bytes_read <= 0) {
        close(client_socket);
        pthread_exit(NULL);
    }
    
    char* cmd = buffer;
    clean_command(cmd);
    
    // createtracker part
    if (strncmp(cmd, "createtracker", 13) == 0) {
        char filename[256], desc[256], md5[256], ip[64];
        long long filesize;
        int port;
        
        // Parse the command and strictly validate all inputs
        if (sscanf(cmd, "createtracker %255s %lld %255s %255s %63s %d", 
                   filename, &filesize, desc, md5, ip, &port) == 6 &&
                   filename[0] != '.' && !strchr(filename, '/') && !strstr(filename, "..") &&
                   filesize >= 0 && port > 0 && port <= 65535 && strlen(md5) == 32 &&
                   inet_pton(AF_INET,ip,&(struct in_addr){0}) == 1) {
            
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s.track", shared_dir, filename);
            
            pthread_mutex_lock(&file_mutex);
            if (access(path, F_OK) != -1) {
                // File already exists
                send_reply(client_socket, "<createtracker ferr>\n");
            } else {
                FILE* f = fopen(path, "w");
                if (f) {
                    // Write the initial metadata and the seeding peer's record
                    fprintf(f, "Filename: %s\n", filename);
                    fprintf(f, "Filesize: %lld\n", filesize);
                    fprintf(f, "Description: %s\n", desc);
                    fprintf(f, "MD5: %s\n", md5);
                    fprintf(f, "#list of peers follows next\n");
                    fprintf(f, "%s:%d:0:%lld:%ld\n", ip, port, filesize, time(NULL));
                    fclose(f);
                    send_reply(client_socket, "<createtracker succ>\n");
                } else {
                    send_reply(client_socket, "<createtracker fail>\n");
                }
            }
            pthread_mutex_unlock(&file_mutex);
        } else {
            send_reply(client_socket, "<createtracker fail>\n");
        }
    }
    // updatetracker part
    else if (strncmp(cmd, "updatetracker", 13) == 0) {
        char filename[256], ip[64];
        long long start_bytes, end_bytes;
        int port;
        
        // Parse and validate the update request
        if (sscanf(cmd, "updatetracker %255s %lld %lld %63s %d", 
                   filename, &start_bytes, &end_bytes, ip, &port) == 5 &&
                   filename[0] != '.' && !strchr(filename, '/') && !strstr(filename,"..") &&
                   start_bytes >= 0 && end_bytes >= start_bytes &&
                   port>0 && port<=65535 && inet_pton(AF_INET,ip,&(struct in_addr){0})==1) {
            
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s.track", shared_dir, filename);
            
            char fail_reply[512], succ_reply[512], ferr_reply[512];
            snprintf(fail_reply, sizeof(fail_reply), "<updatetracker %s fail>\n", filename);
            snprintf(succ_reply, sizeof(succ_reply), "<updatetracker %s succ>\n", filename);
            snprintf(ferr_reply, sizeof(ferr_reply), "<updatetracker %s ferr>\n", filename);

            pthread_mutex_lock(&file_mutex);
            if (access(path, F_OK) == -1) {
                // Tracker file does not exist
                send_reply(client_socket, ferr_reply);
            } else {
                // Call replacement function to update peer details safely
                if (replace_peer_record(path,ip,port,start_bytes,end_bytes)==0) {
                    send_reply(client_socket, succ_reply);
                } else {
                    send_reply(client_socket, fail_reply);
                }
            }
            pthread_mutex_unlock(&file_mutex);
        } else {
            // Send failure message even if filename parsing partially fails
            char fail_msg[256];
            char fname[256] = "unknown";
            sscanf(cmd, "updatetracker %255s", fname);
            snprintf(fail_msg, sizeof(fail_msg), "<updatetracker %s fail>\n", fname);
            send_reply(client_socket, fail_msg);
        }
    }
    // REQ LIST part
    else if (strncmp(cmd, "REQ LIST", 8) == 0) {
        pthread_mutex_lock(&file_mutex);
        DIR *d;
        struct dirent *dir;
        d = opendir(shared_dir);
        if (d) {
            int count = 0;
            // First pass counts the total number of .track files
            while ((dir = readdir(d)) != NULL) {
                if (strstr(dir->d_name, ".track")) count++;
            }
            rewinddir(d);
            
            char reply[1024];
            snprintf(reply, sizeof(reply), "<REP LIST %d>\n", count);
            send_reply(client_socket, reply);
            
            int i = 1;
            // Second pass parses each file's metadata and send it to the client
            while ((dir = readdir(d)) != NULL) {
                if (strstr(dir->d_name, ".track")) {
                    char path[1024];
                    snprintf(path, sizeof(path), "%s/%s", shared_dir, dir->d_name);
                    FILE* f = fopen(path, "r");
                    if (f) {
                        char fname[256] = {0}, fmd5[256] = {0};
                        long long fsize = 0;
                        char line[256];

                        // Extract required fields from the tracker file header
                        while(fgets(line, sizeof(line), f)) {
                            if (strncmp(line, "Filename:", 9) == 0)
                                sscanf(line, "Filename: %s", fname);
                            else if (strncmp(line, "Filesize:", 9) == 0)
                                sscanf(line, "Filesize: %lld", &fsize);
                            else if (strncmp(line, "MD5:", 4) == 0)
                                sscanf(line, "MD5: %s", fmd5);
                            else if (strncmp(line, "#", 1) == 0)
                                break;
                        }
                        fclose(f);
                        snprintf(reply, sizeof(reply), "<%d %s %lld %s>\n", i++, fname, fsize, fmd5);
                        send_reply(client_socket, reply);
                    }
                }
            }
            closedir(d);
            send_reply(client_socket, "<REP LIST END>\n");
        }
        pthread_mutex_unlock(&file_mutex);
    }
    // GET part
    else if (strncmp(cmd, "GET ", 4) == 0) {
        char filename[256];
        char extra;
        // Validate filename ends in .track and prevents directory traversal
        if (sscanf(cmd, "GET %255s %c", filename, &extra) == 1 &&
            strlen(filename) > 6 &&
            strcmp(filename + strlen(filename) - 6, ".track") == 0 &&
            filename[0] != '.' && !strchr(filename, '/') &&
            !strchr(filename, '\\') && !strstr(filename, "..")) {
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s", shared_dir, filename);
            pthread_mutex_lock(&file_mutex);
            FILE *f = fopen(path, "rb");
            if (f) {
                // Determine file size
                if (fseek(f, 0, SEEK_END) == 0) {
                    long length = ftell(f);
                    // Enforce a maximum file size limit to prevent memory from overloading
                    if (length >= 0 && length <= 1024*1024 && fseek(f, 0, SEEK_SET) == 0) {
                        unsigned char *data = malloc((size_t)length + 1);
                        // Read the entire file into memory
                        if (data && fread(data, 1, (size_t)length, f) == (size_t)length) {
                            unsigned char digest[EVP_MAX_MD_SIZE];
                            unsigned int digest_len = 0;
                            // Generate a hash of the tracker file contents
                            if (EVP_Digest(data,(size_t)length,digest,&digest_len,EVP_md5(),NULL)
                                && digest_len == 16) {
                                char end_reply[128];
                                char hexdigest[33];
                                // Convert binary hash to a hex string
                                for (int i=0;i<16;++i) snprintf(hexdigest+2*i,3,"%02x",digest[i]);
                                snprintf(end_reply,sizeof(end_reply),"<REP GET END %s>\n",hexdigest);
                                // Send begin tag, full file data, and end tag with the calculated hash
                                if (net_send_all(client_socket,"<REP GET BEGIN>\n",16) == 0 &&
                                    net_send_all(client_socket,data,(size_t)length) == 0) {
                                    (void)net_send_all(client_socket,end_reply,strlen(end_reply));
                                }
                            }
                        }
                        free(data);
                    }
                }
                fclose(f);
            } else {
                send_reply(client_socket, "<GET fail>\n");
            }
            pthread_mutex_unlock(&file_mutex);
        } else {
            send_reply(client_socket, "<GET fail>\n");
        }
    }
    
    close(client_socket); // Clean up the connection
    pthread_exit(NULL); // Terminate the worker thread
}

int main() {
    // Read configuration from 'sconfig'
    FILE* config = fopen("sconfig", "r");
    if (!config) {
        perror("Error reading sconfig");
        return 1;
    }
    // Port on line 1, Directory on line 2
    if (fscanf(config, "%d\n%255s", &server_port, shared_dir) != 2)
    {
        fprintf(stderr, "Error reading sconfig");
        fclose(config);
        return 1;
    }
    fclose(config);
    
    // Ensure the shared directory exists
    struct stat st = {0};
    if (stat(shared_dir, &st) == -1) {
        mkdir(shared_dir, 0777);
    }

    int server_fd;
    struct sockaddr_in address;
    
    // Create the TCP socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket failed");
        return 1;
    }
    
    // Set socket options to allow immediate reuse of the port after restart
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // Bind the socket to the port found in sconfig
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(server_port);
    
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        return 1;
    }
    
    // Listen for incoming connections
    if (listen(server_fd, 10) < 0) {
        perror("Listen failed");
        return 1;
    }
    
    printf("Tracker server listening on port %d...\n", server_port);
    
    // Main server loop: accept connections and dispatch threads
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        // Dynamically allocate memory for the socket descriptor to pass to the thread safely
        int* client_socket = malloc(sizeof(int));
        
        *client_socket = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (*client_socket < 0) {
            perror("Accept failed");
            free(client_socket);
            continue;
        }
        
        // Hand the connection off to a worker thread
        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_client, client_socket) != 0) {
            perror("Failed to create thread");
            close(*client_socket);
            free(client_socket);
        } else {
            // Detach the thread so its resources are automatically reclaimed upon exit
            pthread_detach(thread_id);
        }
    }
    
    return 0;
}
