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

volatile sig_atomic_t server_running = 1;

static void signal_handler(int signum)
{
    if (signum != SIGINT && signum != SIGTERM)
    {
        return;
    }
    syslog(LOG_USER | LOG_DEBUG, "Caught signal, exiting");
    server_running = 0;
}

static void alarm_handler(int sig, siginfo_t *si, void *uc)
{
    if (sig != SIGALRM)
    {
        return;
    }
    syslog(LOG_USER | LOG_DEBUG, "Alarm triggered, writing timestamp to file.");
    (void)sig;
    (void)uc;
    server_info_t *server_info = (server_info_t *)si->si_value.sival_ptr;
    pthread_mutex_lock(&server_info->file_mutex);
    // Based on example from https://man7.org/linux/man-pages/man3/strftime.3.html
    // Per assignment, use RFC 2822 format.
    char timestamp[100];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "timestamp:%a, %d %b %Y %T %z", tm_info);
    // Append timestamp to file with newline.
    int fd = open(FILENAME, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0)
    {
        perror("open");
        syslog(LOG_USER | LOG_ERR, "Error opening file <%s> for timestamp writing [%s]", FILENAME, strerror(errno));
        pthread_mutex_unlock(&server_info->file_mutex);
        return;
    }
    dprintf(fd, "%s\n", timestamp);
    close(fd);

    pthread_mutex_unlock(&server_info->file_mutex);
}

int main(int argc, char *argv[])
{
    if (argc > 2)
    {
        fprintf(stdout, "Usage: %s [-d]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Open syslog for logging
    openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER);

    server_info_t server_info;
    SLIST_INIT(&server_info.conn_node_head);

    // Shared mutex for output file synchronization.
    pthread_mutex_t file_mutex;
    pthread_mutex_init(&file_mutex, NULL);
    server_info.file_mutex = file_mutex;

    int sockfd, clfd;
    socklen_t clilen;
    struct sockaddr_in serv_addr, cli_addr;

    // Create socket for IPv4
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        perror("socket");
        syslog(LOG_USER | LOG_ERR, "Error opening socket [%s]", strerror(errno));
        exit(EXIT_FAILURE);
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
        exit(EXIT_FAILURE);
    }

    // Daemonize if the "-d" arg is set. LSP Ch. 5.
    if (argc > 1 && strcmp(argv[1], "-d") == 0)
    {
        if (daemon(0, 0) < 0)
        {
            perror("daemon");
            syslog(LOG_USER | LOG_ERR, "Error daemonizing server process [%s]", strerror(errno));
            close(sockfd);
            exit(EXIT_FAILURE);
        }
    }

    // Set up our signal handler for SIGINT or SIGTERM.
    struct sigaction new_action;
    memset(&new_action, 0, sizeof(struct sigaction));
    new_action.sa_handler = signal_handler;
    if (sigaction(SIGINT, &new_action, NULL) != 0)
    {
        perror("Setting SIGINT handler");
        syslog(LOG_USER | LOG_ERR, "Error registering SIGINT handler [%s]", strerror(errno));
        exit(EXIT_FAILURE);
    }
    if (sigaction(SIGTERM, &new_action, NULL) != 0)
    {
        perror("Setting SIGTERM handler");
        syslog(LOG_USER | LOG_ERR, "Error registering SIGTERM handler [%s]", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Set up signal handler for SIGALRM for timestamp writing.
    struct sigaction alrm_action;
    memset(&alrm_action, 0, sizeof(struct sigaction));
    alrm_action.sa_sigaction = alarm_handler;
    alrm_action.sa_flags = SA_SIGINFO | SA_RESTART; // SA_RESTART to allow accept() below to be restarted.
    if (sigaction(SIGALRM, &alrm_action, NULL) != 0)
    {
        perror("Setting SIGALRM handler");
        syslog(LOG_USER | LOG_ERR, "Error registering SIGALRM handler [%s]", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Set up timer for SIGALRM every 10 seconds.
    struct itimerspec timer_spec;
    timer_spec.it_value.tv_sec = 10;
    timer_spec.it_value.tv_nsec = 0;
    timer_spec.it_interval.tv_sec = 10;
    timer_spec.it_interval.tv_nsec = 0;
    struct sigevent sev;
    memset(&sev, 0, sizeof(struct sigevent));
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGALRM;
    // Pass pointer to server_info so we can reference the file mutex and any other shared state in
    // the timer handler.
    sev.sigev_value.sival_ptr = &server_info;
    timer_t timer_id;
    if (timer_create(CLOCK_MONOTONIC, &sev, &timer_id) != 0)
    {
        perror("timer_create");
        syslog(LOG_USER | LOG_ERR, "Error creating timer for SIGALRM [%s]", strerror(errno));
        exit(EXIT_FAILURE);
    }
    if (timer_settime(timer_id, 0, &timer_spec, NULL) != 0)
    {
        perror("timer_settime");
        syslog(LOG_USER | LOG_ERR, "Error setting timer for SIGALRM [%s]", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Listen for incoming connections. Backlog is 5 for up to 5 pending connections.
    listen(sockfd, 5);
    syslog(LOG_USER | LOG_DEBUG, "Server started on port %d", SERVER_PORT);

    while (server_running)
    {
        clilen = sizeof(cli_addr);
        clfd = accept(sockfd, (struct sockaddr *)&cli_addr, &clilen);
        if (clfd < 0)
        {
            // Check for accept() interruption due to signal. Break out if so and
            // we're shutting down.
            if (errno == EINTR && !server_running)
            {
                break;
            }
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
        SLIST_INSERT_HEAD(&server_info.conn_node_head, new_node, nodes);

        // Create thread to handle the connection. Pass pointer to conn_node as arg.
        if (pthread_create(&new_node->thread_id, NULL, handle_connection, new_node) != 0)
        {
            perror("pthread_create");
            syslog(LOG_USER | LOG_ERR, "Error creating thread for new connection [%s]", strerror(errno));
            SLIST_REMOVE(&server_info.conn_node_head, new_node, conn_node, nodes);
            free(new_node);
            close(clfd);
            continue;
        }

        // Use SLIST_FOREACH_SAFE to iterate through the list and clean up any nodes whose threads have completed.
        conn_node_t *cur_node, *tmp_node;
        SLIST_FOREACH_SAFE(cur_node, &server_info.conn_node_head, nodes, tmp_node)
        {
            if (cur_node->done)
            {
                pthread_join(cur_node->thread_id, NULL);
                SLIST_REMOVE(&server_info.conn_node_head, cur_node, conn_node, nodes);
                free(cur_node);
            }
        }
    }

    // Final cleanup of any remaining connection nodes and threads.
    conn_node_t *cur_node, *tmp_node;
    SLIST_FOREACH_SAFE(cur_node, &server_info.conn_node_head, nodes, tmp_node)
    {
        if (cur_node->done)
        {
            pthread_join(cur_node->thread_id, NULL);
            SLIST_REMOVE(&server_info.conn_node_head, cur_node, conn_node, nodes);
            free(cur_node);
        }
    }
    close(sockfd);
    pthread_mutex_destroy(&file_mutex);
    if (unlink(FILENAME) < 0)
    {
        perror("unlink");
        syslog(LOG_USER | LOG_ERR, "Error unlinking file <%s> [%s]", FILENAME, strerror(errno));
    }
    closelog();
    return 0;
}
