#ifndef AESDSOCKET_H
#define AESDSOCKET_H

#include <stddef.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "queue.h"

#define SERVER_PORT 9000
#define BUFFER_SIZE 1024
#define FILENAME "/var/tmp/aesdsocketdata"

typedef struct conn_node
{
    // Thread ID and client address for this connection.
    pthread_t thread_id;
    struct sockaddr_in cli_addr;

    // Client socket file descriptor for this connection.
    int clfd;

    // Mutex for output file access. Passed by the caller.
    pthread_mutex_t *file_mutex;

    // Indicates whether this connection has completed processing
    // and can be safely cleaned up.
    bool done;

    SLIST_ENTRY(conn_node)
    nodes;
} conn_node_t;

#endif