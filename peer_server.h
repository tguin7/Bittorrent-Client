#ifndef CS4390_PEER_SERVER_H
#define CS4390_PEER_SERVER_H

// thread_server callback interface
void peer_serve_connection(int client_fd, void *shared_dir_context);

#endif
