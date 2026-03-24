/**
 * aesdsocket.c
 *
 * Server for Assignment 5
 */

#define _GNU_SOURCE // https://github.com/Microsoft/vscode/issues/71012
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <syslog.h>
#include <fcntl.h>
#include <errno.h>

#include "aesdsocket.h"
#include "handler.h"

typedef SLIST_HEAD(conn_node_s, conn_node) conn_node_head_t;

static void signal_handler(int signum)
{
    int errno_saved = errno;
    syslog(LOG_USER | LOG_DEBUG, "Caught signal, exiting");
    if (unlink(FILENAME) < 0)
    {
        perror("unlink");
        syslog(LOG_USER | LOG_ERR, "Error unlinking file <%s> [%s]", FILENAME, strerror(errno));
    }
    errno = errno_saved;
    exit(EXIT_SUCCESS);
}

int main(int argc, char *argv[])
{
    if (argc > 2)
    {
        fprintf(stdout, "Usage: %s [-d]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Singly-linked list for client handler thread tracking.
    conn_node_head_t conn_node_head;
    SLIST_INIT(&conn_node_head);

    // Shared mutex for output file synchronization.
    pthread_mutex_t file_mutex;
    pthread_mutex_init(&file_mutex, NULL);

    int sockfd, clfd;
    socklen_t clilen;
    struct sockaddr_in serv_addr, cli_addr;

    // Set up our signal handler for SIGINT or SIGTERM.
    struct sigaction new_action;
    memset(&new_action, 0, sizeof(struct sigaction));
    new_action.sa_handler = signal_handler;
    if (sigaction(SIGINT, &new_action, NULL) != 0)
    {
        perror("Setting SIGINT handler");
        syslog(LOG_USER | LOG_ERR, "Error registering SIGINT handler [%s]", strerror(errno));
    }
    if (sigaction(SIGTERM, &new_action, NULL) != 0)
    {
        perror("Setting SIGTERM handler");
        syslog(LOG_USER | LOG_ERR, "Error registering SIGTERM handler [%s]", strerror(errno));
    }

    // Open syslog for logging
    openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER);

    // Create socket for IPv4
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        perror("socket");
        syslog(LOG_USER | LOG_ERR, "Error opening socket [%s]", strerror(errno));
        return 1;
    }

    // Set up server address structure explicitly for IPv4.
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(SERVER_PORT);

    // Bind socket to address. Need to cast sockaddr_in to sockaddr.
    if (bind(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        perror("bind");
        syslog(LOG_USER | LOG_ERR, "bind() error [%s]", strerror(errno));
        close(sockfd);
        return 1;
    }

    // Daemonize if the "-d" arg is set. LSP Ch. 5.
    if (argc > 1 && strcmp(argv[1], "-d") == 0)
    {
        if (daemon(0, 0) < 0)
        {
            perror("daemon");
            syslog(LOG_USER | LOG_ERR, "Error daemonizing server process [%s]", strerror(errno));
            close(sockfd);
            return 1;
        }
    }

    // Listen for incoming connections. Backlog is 5 for up to 5 pending connections.
    listen(sockfd, 5);
    syslog(LOG_USER | LOG_DEBUG, "Server started on port %d", SERVER_PORT);

    while (1)
    {
        clilen = sizeof(cli_addr);
        clfd = accept(sockfd, (struct sockaddr *)&cli_addr, &clilen);
        if (clfd < 0)
        {
            perror("accept");
            syslog(LOG_USER | LOG_ERR, "accept() error [%s]", strerror(errno));
            continue; // Try to continue accepting new connections.
        }
        syslog(LOG_USER | LOG_DEBUG, "Accepted connection from %s", inet_ntoa(cli_addr.sin_addr));

        // Create new conn_node for the connection, set fields, and add to list.
        conn_node_t *new_node = malloc(sizeof(conn_node_t));
        if (new_node == NULL)
        {
            perror("malloc");
            syslog(LOG_USER | LOG_ERR, "Error allocating memory for new connection node [%s]", strerror(errno));
            close(clfd);
            continue;
        }
        new_node->cli_addr = cli_addr;
        new_node->clfd = clfd;
        new_node->file_mutex = &file_mutex;
        new_node->done = false;
        SLIST_INSERT_HEAD(&conn_node_head, new_node, nodes);

        // Create thread to handle the connection. Pass pointer to conn_node as arg.
        if (pthread_create(&new_node->thread_id, NULL, handle_connection, new_node) != 0)
        {
            perror("pthread_create");
            syslog(LOG_USER | LOG_ERR, "Error creating thread for new connection [%s]", strerror(errno));
            SLIST_REMOVE(&conn_node_head, new_node, conn_node, nodes);
            free(new_node);
            close(clfd);
            continue;
        }

        // Use SLIST_FOREACH_SAFE to iterate through the list and clean up any nodes whose threads have completed.
        conn_node_t *cur_node, *tmp_node;
        SLIST_FOREACH_SAFE(cur_node, &conn_node_head, nodes, tmp_node)
        {
            if (cur_node->done)
            {
                pthread_join(cur_node->thread_id, NULL);
                SLIST_REMOVE(&conn_node_head, cur_node, conn_node, nodes);
                free(cur_node);
            }
        }
    }

    pthread_mutex_destroy(&file_mutex);
    closelog();
    return 0;
}
