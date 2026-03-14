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

#define SERVER_PORT 9000
#define BUFFER_SIZE 1024
#define FILENAME "/var/tmp/aesdsocketdata"

static void signal_handler(int signum)
{
    int errno_saved = errno;
    syslog(LOG_USER | LOG_INFO, "Caught signal, exiting");
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
    int sockfd, clfd;
    socklen_t clilen;
    char read_buffer[BUFFER_SIZE];
    struct sockaddr_in serv_addr, cli_addr;
    ssize_t n;

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

    // Bind socket to address.
    if (bind(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        perror("bind");
        syslog(LOG_USER | LOG_ERR, "bind() error [%s]", strerror(errno));
        close(sockfd);
        return 1;
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

        // Buffer to store data up to a newline character. We will read data in chunks and write to file when a newline is encountered.
        // TODO: Use valgrind to check for memory leaks.
        char *data_buffer = malloc(BUFFER_SIZE);
        if (data_buffer == NULL)
        {
            perror("Error allocating buffer");
            syslog(LOG_USER | LOG_ERR, "Error allocating buffer [%s]", strerror(errno));
            close(clfd);
            continue; // Try to continue accepting new connections.
        }
        size_t data_len = 0;

        // Read data from client and write to file
        while ((n = recv(clfd, read_buffer, BUFFER_SIZE - 1, 0)) > 0)
        {
            // Store data in a buffer and write to file when a newline is encountered. If we exceed the data buffer size,
            // we need to malloc a larger buffer and copy the existing data over.
            for (ssize_t i = 0; i < n; i++)
            {
                if (data_len < BUFFER_SIZE - 1)
                {
                    data_buffer[data_len++] = read_buffer[i];
                }
                else
                {
                    // Buffer overflow, we need to allocate a larger buffer.
                    size_t new_size = data_len + BUFFER_SIZE;
                    char *new_buffer = realloc(data_buffer, new_size);
                    if (new_buffer == NULL)
                    {
                        perror("Error reallocating buffer");
                        syslog(LOG_USER | LOG_ERR, "Error reallocating buffer [%s]", strerror(errno));
                        break; // Break out of the loop to close the connection
                    }
                    data_buffer = new_buffer;
                    data_buffer[data_len++] = read_buffer[i];
                }

                // If we encounter a newline character, write the data "packet" to file and reset the buffer.
                if (read_buffer[i] == '\n')
                {
                    // Null-terminate the data buffer before writing to file.
                    data_buffer[data_len] = '\0';

                    // Append data to file
                    int fd = open(FILENAME, O_WRONLY | O_CREAT | O_APPEND, 0644);
                    if (fd == -1)
                    {
                        perror("Error opening file (filename: " FILENAME ")");
                        syslog(LOG_USER | LOG_ERR, "Error opening file [%s]: %s", strerror(errno), FILENAME);
                        break;
                    }
                    ssize_t res = write(fd, data_buffer, data_len);
                    if (res == -1)
                    {
                        perror("Error writing to file (filename: " FILENAME ")");
                        syslog(LOG_USER | LOG_ERR, "Error writing to file [%s]: %s", strerror(errno), FILENAME);
                        close(fd);
                        break;
                    }
                    close(fd);
                    // syslog(LOG_USER | LOG_DEBUG, "Wrote <%s> to file <%s>", data_buffer, FILENAME);
                    // Reset data buffer for next line of input.
                    data_len = 0;
                    free(data_buffer);
                    data_buffer = malloc(BUFFER_SIZE);
                    if (data_buffer == NULL)
                    {
                        perror("Error allocating buffer");
                        syslog(LOG_USER | LOG_ERR, "Error allocating buffer [%s]", strerror(errno));
                        break;
                    }

                    // Write full file contents back to client.
                    fd = open(FILENAME, O_RDONLY);
                    if (fd < 0)
                    {
                        perror("open");
                        syslog(LOG_USER | LOG_ERR, "Error opening file (%s) [%s]", FILENAME, strerror(errno));
                        close(clfd);
                        continue;
                    }
                    while ((n = read(fd, read_buffer, BUFFER_SIZE)) > 0)
                    {
                        if (n < 0)
                        {
                            perror("read");
                            syslog(LOG_USER | LOG_ERR, "Error reading file (%s) [%s]", FILENAME, strerror(errno));
                            break;
                        }
                        // printf("Sending <%s> to client\n", read_buffer);
                        ssize_t sent = send(clfd, read_buffer, n, 0);
                        if (sent != n)
                        {
                            syslog(LOG_USER | LOG_ERR, "Read (%ld) and send (%ld) mismatch", n, sent);
                            break;
                        }
                    }
                    close(fd);
                }
            }
        }
        if (n < 0)
        {
            perror("Error reading from socket");
            syslog(LOG_USER | LOG_ERR, "Error reading from socket [%s]", strerror(errno));
            close(clfd);
            continue;
        }

        close(clfd);
        syslog(LOG_USER | LOG_DEBUG, "Closed connection from %s", inet_ntoa(cli_addr.sin_addr));
    }
    closelog();
    return 0;
}