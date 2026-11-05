#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>

//send all requested bytes to peer
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

//1 thread per peer
void *handle_peer(void *arg)
{	
	// retrieve peer socket
	int client_skt = *(int *)arg;

	free(arg);

	// buffer that stores request
	char buffer[1024];
	size_t bytes_rcvd = 0;

	//read request
	while (bytes_rcvd < sizeof(buffer) - 1)
	{
		char ch;

		// receive 1 character at a time
		ssize_t n = recv(client_skt, &ch, 1, 0);


		if (n < 0 && errno == EINTR)
		{
			continue;
		}
		else if (n <= 0) // peer disconnected or recv fails
		{
			goto done;
		}
		else if (ch == '\n')
		{
			break;
		}
		else
		{
			// store rcvd char
			buffer[bytes_rcvd++] = ch;
		}
	}

	// reject request that are bigger than buffer
	if (bytes_rcvd == sizeof(buffer) - 1)
	{ goto invalid; }

	buffer[bytes_rcvd] = '\0';

	char filename[128];
	char filepath[256];
	long long first_byte;
	long long chunk_size;
	char extra;

	//extract filename, first byte and chunk size
	int parsed = sscanf(buffer, "GET %127s %lld %lld %c", filename, &first_byte, &chunk_size, &extra);

	// validate request
	if (parsed != 3 || first_byte < 0 || first_byte > LONG_MAX || chunk_size <= 0 || chunk_size > 1024 || strchr(filename, '/') != NULL || strchr(filename, '\\') != NULL || strstr(filename, "..") != NULL)
	{
		goto invalid;
	}

	// make path for requested file
	snprintf(filepath, sizeof(filepath), "shared/%s", filename);

	// open file
	FILE *file = fopen(filepath, "rb");


	if (file == NULL)
	{	
		// file didnt open
		perror("Failed to open file");
		send_bytes(client_skt, "<GET missing>\n", 14);
		goto done;
	}
	else
	{
		// file ptr moved to first byte
		if (fseek(file, (long)first_byte, SEEK_SET) != 0)
		{
			perror("File seek failed");
			fclose(file);
			goto invalid;
		}
		else
		{
			// store req file chunk
			char file_buffer[1024];

			//read up to chunk size
			size_t bytes_read = fread(file_buffer, 1, chunk_size, file);

			if (ferror(file))
			{
				perror("File read failed");
				fclose(file);
				goto done;
			}
			else
			{
				printf("Peer requested %lld bytes from %s at %lld\n", chunk_size, filename, first_byte);

				// send requested file bytes to peer
				if (send_bytes(client_skt, file_buffer, bytes_read) == -1)
				{
					perror("File send failed");
				}
				else
				{
					printf("Sent %zu bytes from %s\n", bytes_read, filename);
				}
			}
		fclose(file);
		goto done;
	
		}
	}
	// reject invalid req or oversized chunks
	invalid:
		send_bytes(client_skt, "<GET invalid>\n", 14);
		printf("Invalid GET request.\n");

	done:
		// close connection with peer
		close(client_skt);
		return NULL;
}


int main(void)
{
	// socket file descriptor
	int server_socket;
	// store IP information
	struct sockaddr_in server_addr;

	//prevent terminating on broken sockets
	signal(SIGPIPE, SIG_IGN);

	// create tcp skt
	server_socket = socket(AF_INET, SOCK_STREAM, 0);

	if (server_socket == -1)
	{
		perror("Failed to create socket");
		return 1;
	}

	// server port can be reused after restarting
	int opt = 1;
	if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1)
	{
		perror("setsockopt failed");
		close(server_socket);
		return 1;
	}

	//clear server structure 
	memset(&server_addr, 0, sizeof(server_addr));

	//configure IP, port and interface
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(5001);
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
	{
		perror("Bind failed");
		close(server_socket);
		return 1;
	}
	printf("Socket bound to port 5001\n");
	

	if (listen(server_socket, 10) == -1)
	{
		perror("Listen failed");
		close(server_socket);
		return 1;
	}

	printf("Peer server listening on port 5001 \n");

	while (1)
	{
		// store info about peer
		struct sockaddr_in client_addr;
		socklen_t client_len = sizeof(client_addr);

		//wait and accept incoming TCP connection
		int client_skt = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);

		//for connection errors
		if (client_skt == -1)
		{
			if (errno == EINTR)
			{ continue; }
			printf("Accept failed");
			continue;
		}

		printf("A peer connected\n");

		// memory for peer skt descriptor
		int *skt_ptr = malloc(sizeof(int));

		if (skt_ptr == NULL)
		{
			perror("Memory allocation failed");
			close(client_skt);
			continue;
		}

		// store skt in memory
		*skt_ptr = client_skt;

		//new thread for connected peer
		pthread_t thread_id;

		if (pthread_create(&thread_id, NULL, handle_peer, skt_ptr) != 0)
		{
			fprintf(stderr, "thread creation failed\n");
			free(skt_ptr);
			close(client_skt);
			continue;
		}

		pthread_detach(thread_id);
	}

	close(server_socket);

	return 0;
}
