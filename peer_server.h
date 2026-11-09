#ifndef CS4390_PEER_SERVER_H
#define CS4390_PEER_SERVER_H

/* Member 3's file-serving logic, adapted by Member 4 to the shared
 * thread_server callback interface. The callback never closes the socket. */
void peer_serve_connection(int client_fd, void *shared_dir_context);

#endif
