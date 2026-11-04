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

#define BUFFER_SIZE 2048

int server_port = 0;
char shared_dir[256];
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

void send_reply(int socket, const char* msg) {
    send(socket, msg, strlen(msg), 0);
}

// Helper to remove brackets and newlines from manually typed commands
void clean_command(char* cmd) {
    if (cmd[0] == '<') memmove(cmd, cmd + 1, strlen(cmd));
    char *end = strchr(cmd, '>');
    if (end) *end = '\0';
    end = strchr(cmd, '\n');
    if (end) *end = '\0';
}

void* handle_client(void* arg) {
    int client_socket = *(int*)arg;
    free(arg);
    
    char buffer[BUFFER_SIZE];
    memset(buffer, 0, BUFFER_SIZE);
    
    // Using recv() as recommended by Beej's Guide
    int bytes_read = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
    if (bytes_read <= 0) {
        close(client_socket);
        pthread_exit(NULL);
    }
    
    char* cmd = buffer;
    clean_command(cmd);
    
    if (strncmp(cmd, "createtracker", 13) == 0) {
        char filename[256], desc[256], md5[256], ip[64];
        long long filesize;
        int port;
        
        if (sscanf(cmd, "createtracker %255s %lld %255s %255s %63s %d", 
                   filename, &filesize, desc, md5, ip, &port) == 6) {
            
            char path[512];
            snprintf(path, sizeof(path), "%s/%s.track", shared_dir, filename);
            
            pthread_mutex_lock(&file_mutex);
            if (access(path, F_OK) != -1) {
                send_reply(client_socket, "<createtracker ferr>\n");
            } else {
                FILE* f = fopen(path, "w");
                if (f) {
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
    else if (strncmp(cmd, "updatetracker", 13) == 0) {
        char filename[256], ip[64];
        long long start_bytes, end_bytes;
        int port;
        
        if (sscanf(cmd, "updatetracker %255s %lld %lld %63s %d", 
                   filename, &start_bytes, &end_bytes, ip, &port) == 5) {
            
            char path[512];
            snprintf(path, sizeof(path), "%s/%s.track", shared_dir, filename);
            
            char fail_reply[256], succ_reply[256], ferr_reply[256];
            snprintf(fail_reply, sizeof(fail_reply), "<updatetracker %s fail>\n", filename);
            snprintf(succ_reply, sizeof(succ_reply), "<updatetracker %s succ>\n", filename);
            snprintf(ferr_reply, sizeof(ferr_reply), "<updatetracker %s ferr>\n", filename);

            pthread_mutex_lock(&file_mutex);
            if (access(path, F_OK) == -1) {
                send_reply(client_socket, ferr_reply);
            } else {
                FILE* f = fopen(path, "a");
                if (f) {
                    fprintf(f, "%s:%d:%lld:%lld:%ld\n", ip, port, start_bytes, end_bytes, time(NULL));
                    fclose(f);
                    send_reply(client_socket, succ_reply);
                } else {
                    send_reply(client_socket, fail_reply);
                }
            }
            pthread_mutex_unlock(&file_mutex);
        } else {
            char fail_msg[256];
            char fname[256] = "unknown";
            sscanf(cmd, "updatetracker %255s", fname);
            snprintf(fail_msg, sizeof(fail_msg), "<updatetracker %s fail>\n", fname);
            send_reply(client_socket, fail_msg);
        }
    }
    else if (strncmp(cmd, "REQ LIST", 8) == 0) {
        pthread_mutex_lock(&file_mutex);
        DIR *d;
        struct dirent *dir;
        d = opendir(shared_dir);
        if (d) {
            int count = 0;
            while ((dir = readdir(d)) != NULL) {
                if (strstr(dir->d_name, ".track")) count++;
            }
            rewinddir(d);
            
            char reply[1024];
            snprintf(reply, sizeof(reply), "<REP LIST %d>\n", count);
            send_reply(client_socket, reply);
            
            int i = 1;
            while ((dir = readdir(d)) != NULL) {
                if (strstr(dir->d_name, ".track")) {
                    char path[512];
                    snprintf(path, sizeof(path), "%s/%s", shared_dir, dir->d_name);
                    FILE* f = fopen(path, "r");
                    if (f) {
                        char fname[256] = {0}, fmd5[256] = {0};
                        long long fsize = 0;
                        char line[256];
                        while(fgets(line, sizeof(line), f)) {
                            if (strncmp(line, "Filename:", 9) == 0) sscanf(line, "Filename: %s", fname);
                            else if (strncmp(line, "Filesize:", 9) == 0) sscanf(line, "Filesize: %lld", &fsize);
                            else if (strncmp(line, "MD5:", 4) == 0) sscanf(line, "MD5: %s", fmd5);
                            else if (strncmp(line, "#", 1) == 0) break;
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
    else if (strncmp(cmd, "GET ", 4) == 0) {
        char filename[256];
        if (sscanf(cmd, "GET %255s", filename) == 1) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", shared_dir, filename);
            
            pthread_mutex_lock(&file_mutex);
            FILE* f = fopen(path, "r");
            if (f) {
                send_reply(client_socket, "<REP GET BEGIN>\n");
                char line[256];
                char fmd5[256] = "UNKNOWN";
                while(fgets(line, sizeof(line), f)) {
                    send_reply(client_socket, line);
                    if (strncmp(line, "MD5:", 4) == 0) {
                        sscanf(line, "MD5: %s", fmd5);
                    }
                }
                fclose(f);
                char end_reply[256];
                snprintf(end_reply, sizeof(end_reply), "<REP GET END %s>\n", fmd5);
                send_reply(client_socket, end_reply);
            } else {
                send_reply(client_socket, "<GET fail>\n");
            }
            pthread_mutex_unlock(&file_mutex);
        }
    }
    
    close(client_socket);
    pthread_exit(NULL);
}

int main() {
    FILE* config = fopen("sconfig", "r");
    if (!config) {
        perror("Error reading sconfig");
        return 1;
    }
    fscanf(config, "%d\n%255s", &server_port, shared_dir);
    fclose(config);
    
    struct stat st = {0};
    if (stat(shared_dir, &st) == -1) {
        mkdir(shared_dir, 0777);
    }

    int server_fd;
    struct sockaddr_in address;
    
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket failed");
        return 1;
    }
    
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(server_port);
    
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        return 1;
    }
    
    if (listen(server_fd, 10) < 0) {
        perror("Listen failed");
        return 1;
    }
    
    printf("Tracker server listening on port %d...\n", server_port);
    
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int* client_socket = malloc(sizeof(int));
        
        *client_socket = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (*client_socket < 0) {
            perror("Accept failed");
            free(client_socket);
            continue;
        }
        
        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_client, client_socket) != 0) {
            perror("Failed to create thread");
            close(*client_socket);
            free(client_socket);
        } else {
            pthread_detach(thread_id);
        }
    }
    
    return 0;
}
