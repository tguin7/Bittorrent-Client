#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>


// send request
int send_bytes(int skt, const char *data, size_t length)
{
	//track bytes sent
	size_t total = 0;

	//send until all bytes are transmitted
	while (total < length)
	{
		ssize_t sent = send(skt, (data + total), (length - total), MSG_NOSIGNAL);

		if (sent < 0 && errno == EINTR)
		{ 
			continue; 
		}

		//stop sending
		if (sent <= 0)
		{
			return -1; 
		}

		total += (size_t)sent;
	}

	return 0;
}

//download connects to peerServer
static int connect_peer(const char *ip, const char *port)
{
    //specify connection, store address results and move thru list
    struct addrinfo hints, *results = NULL, *p;

    // set bytes to 0
    memset(&hints, 0, sizeof(hints));

    //specify IP and TCP
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    // get address
    int status = getaddrinfo(ip, port, &hints, &results);

    if(status != 0)
    {
        fprintf(stderr, "Address loopup failed %s\n", gai_strerror(status));
        return -1;
    }

    int skt = -1;

    // move thru addresses
    for(p = results; p != NULL; p = p -> ai_next)
    {
        // socket stores info on curr addy
        skt = socket(p->ai_family, p->ai_socktype, p->ai_protocol);

        if(skt < 0)
        {
            continue;
        }

        if(connect(skt, p->ai_addr, p-> ai_addrlen) == 0)
        {
            break;
        }

        close(skt);
        skt = -1;
    }

    freeaddrinfo(results);

    // return connected skt or -1 for failed connection
    return skt;

}

// ask sever for portion of a file
static ssize_t request_chunks(const char *ip, const char *port, const char *filename, unsigned long long offset, size_t chunk_size, char *data)
{
    //establish connection
    int skt = connect_peer(ip, port);

    if(skt < 0)
    {
        fprintf(stderr, "Failed to connect to peer %s:%s\n", ip, port);
        return -1;
    }

    char request[256];

    // create request
    int len = snprintf(request, sizeof(request), "GET %s %llu %zu\n", filename, offset, chunk_size);

    // validate and send GET request
    if(len < 0 || (size_t)len >= sizeof(request) || send_bytes(skt, request, (size_t)len) < 0)
    {
        perror("Sending GET request failed");
        close(skt);
        return -1;
    }
    else
    {
        char response[1024 + 32];

        size_t total = 0;

        //receiver data
        while(total < sizeof(response))
        {
            ssize_t bytes_rcvd = recv(skt, (response + total), (sizeof(response) - total), 0);

            if(bytes_rcvd < 0 && errno == EINTR)
            {
                continue;
            }
            else if(bytes_rcvd < 0)
            {
                perror("Receving failed");
                close(skt);
                return -1;
            }
            
            if(bytes_rcvd == 0)
            {
                break;
            }

            total += (size_t)bytes_rcvd;
        }

        close(skt);

        //check if server error
        if((total == strlen("<GET invalid>\n") && memcmp(response, "<GET invalid>\n", total) == 0) || (total == strlen("<GET missing>\n") &&
                                                                                                      memcmp(response, "<GET missing>\n", total) == 0))
        {
            fprintf(stderr, "Peer rejected because of GET or missing file\n");
            return -1;
        }

        //check if complete chunk arrived
        if(total != chunk_size)
        {
            fprintf(stderr, "Incomplete chunk at %llu: expected %zu, got %zu bytes\n", offset, chunk_size, total);
            return -1;
        }

        //copy bytes to data
        memcpy(data, response, total);

        //bytes received
        return (ssize_t)total;
    }
}

//download chunks and assemble into output file
static int download_file(const char *ip, const char *port, const char *filename, unsigned long long filesize, const char *output)
{
    size_t len = strlen(output);

    if(len > SIZE_MAX - 6)
    {
        return -1;
    }

    //mem for output
    char *temp = malloc(len + 6);

    if(!temp)
    {
        perror("malloc");
        return -1;
    }

    //partial downloads go  to temp
    snprintf(temp, (len + 6), "%s.part", output);

    FILE *file = fopen(temp, "wb");

    if(!file)
    {
        perror("Failed to open output file");
        free(temp);
        return -1;
    }
    else
    {
        char data[1024];
        unsigned long long offset = 0;
        int works = 1;

        //download file chunks
        while(offset < filesize)
        {
            // get bytes that need to be downloaded
            unsigned long long remaining =  (filesize - offset);

            size_t chunk_size;

            if(remaining > 1024)
            {
                chunk_size = 1024;
            }
            else
            {
                chunk_size = (size_t)remaining;
            }

            // get chunks
            ssize_t rcvd = request_chunks(ip, port, filename, offset, chunk_size, data);

            if(rcvd < 0 || fwrite(data, 1, (size_t)rcvd, file) != (size_t)rcvd)
            {
                if(rcvd >= 0)
                {
                    perror("Failed to write output file");
                }

                works = 0;
                break;
            }

            //move offset past chunk
            offset += (unsigned long long)rcvd;

            //current byte count progress
            printf("Downloaded %llu/%llu bytes\n", offset, filesize);
        }

        if(fclose(file) != 0)
        {
            perror("closing output file failed");
            works = 0;
        }

        if(works && rename(temp, output) != 0)
        {
            perror("Failed to rename downloaded file");
            works = 0;
        }

        if(!works)
        {
            unlink(temp);
            fprintf(stderr, "Download failed\n");
        }
        else
        {
            printf("Download complete: %s (%llu bytes)\n", output, filesize);
        }

        free(temp);

        if(works == 1)
        {
            return 0;
        }
        else
        {
            return -1;
        }
    }

}

int main(int argc, char *argv[])
{
    if(argc != 6)
    {
        fprintf(stderr, "Usage: %s PEER_IP PEER_PORT FILENAME FILE_SIZE OUTPUT\n", argv[0]);
        return 1;
    }

    const char *filename = argv[3];

    if(!*filename || strlen(filename) >= 128 || strchr(filename, '/') || strchr(filename, '\\') || strstr(filename, "..") || strchr(filename, ' '))
    {
        fprintf(stderr, "Invalid filename\n");
        return 1;
    }

    errno = 0;
    char *ptr;

    // detect text 
    unsigned long long filesize = strtoull(argv[4], &ptr, 10);

    if(errno || !*argv[4] || *ptr || argv[4][0] == '-' || filesize > LONG_MAX)
    {
        fprintf(stderr, "Invalid file size\n");
        return 1;
    }

    // file download
    if(download_file(argv[1], argv[2], filename, filesize, argv[5]) == 0)
    {
        // worked
        return 0;
    }
    else
    {
        //failed
        return 1;
    }

}