#include <syslog.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>

#include "handler.h"

void *handle_connection(void *arg)
{
    conn_node_t *conn_info = (conn_node_t *)arg;
    char read_buffer[BUFFER_SIZE];
    ssize_t n;

    // Buffer to store data up to a newline character. We will read data in BUFFER_SIZE
    // chunks and write to file when a newline is encountered.
    char *data_buffer = malloc(BUFFER_SIZE);
    if (data_buffer == NULL)
    {
        perror("malloc for data_buffer");
        syslog(LOG_USER | LOG_ERR, "Error allocating buffer [%s]", strerror(errno));
        close(conn_info->clfd);
        conn_info->done = true;
        return NULL;
    }
    size_t data_len = 0;

    // Read data from client and write to file
    while ((n = recv(conn_info->clfd, read_buffer, BUFFER_SIZE - 1, 0)) > 0)
    {
        // Store data in a buffer and write to file when a newline is encountered. If we exceed the data buffer size,
        // we need to malloc a larger buffer and copy the existing data over.
        for (ssize_t i = 0; i < n; i++)
        {
            // Grow data_buffer if necessary.
            if (data_len >= BUFFER_SIZE)
            {
                size_t new_size = data_len + BUFFER_SIZE;
                char *new_buffer = realloc(data_buffer, new_size);
                if (new_buffer == NULL)
                {
                    perror("realloc for data_buffer");
                    syslog(LOG_USER | LOG_ERR, "Error reallocating buffer [%s]", strerror(errno));
                    break; // Break out of the loop to close the connection
                }
                data_buffer = new_buffer;
            }
            data_buffer[data_len++] = read_buffer[i];

            // If we encounter a newline character, write the data "packet" to file and reset the buffer.
            if (read_buffer[i] == '\n')
            {
                // Append data to file
                if (append_packet(FILENAME, data_buffer, data_len, conn_info->file_mutex) != 0)
                {
                    syslog(LOG_USER | LOG_ERR, "Error appending packet to %s", FILENAME);
                    break;
                }
                // syslog(LOG_USER | LOG_DEBUG, "Wrote <%s> to file <%s>", data_buffer, FILENAME);
                // Reset data buffer for next line of input.
                data_len = 0;
                free(data_buffer);
                data_buffer = malloc(BUFFER_SIZE);
                if (data_buffer == NULL)
                {
                    perror("malloc new data_buffer");
                    syslog(LOG_USER | LOG_ERR, "Error allocating buffer [%s]", strerror(errno));
                    break;
                }

                // Write full file contents back to client.
                int fd = open(FILENAME, O_RDONLY);
                if (fd < 0)
                {
                    perror("open");
                    syslog(LOG_USER | LOG_ERR, "Error opening file (%s) [%s]", FILENAME, strerror(errno));
                    close(conn_info->clfd);
                    free(data_buffer);
                    conn_info->done = true;
                    return NULL;
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
                    ssize_t sent = send(conn_info->clfd, read_buffer, n, 0);
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
    free(data_buffer);
    if (n < 0)
    {
        perror("Error reading from socket");
        syslog(LOG_USER | LOG_ERR, "Error reading from socket [%s]", strerror(errno));
        close(conn_info->clfd);
        conn_info->done = true;
        return NULL;
    }

    close(conn_info->clfd);
    syslog(LOG_USER | LOG_DEBUG, "Closed connection from %s", inet_ntoa(conn_info->cli_addr.sin_addr));
    conn_info->done = true;
    return NULL;
}

int append_packet(const char *fname, const char *buf, size_t len, pthread_mutex_t *file_mutex)
{
    pthread_mutex_lock(file_mutex);
    int fd = open(fname, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd == -1)
    {
        perror("Error opening file (filename: " FILENAME ")");
        syslog(LOG_USER | LOG_ERR, "Error opening file [%s]: %s", strerror(errno), FILENAME);
        pthread_mutex_unlock(file_mutex);
        return -1;
    }
    ssize_t res = write(fd, buf, len);
    if (res == -1)
    {
        perror("Error writing to file (filename: " FILENAME ")");
        syslog(LOG_USER | LOG_ERR, "Error writing to file [%s]: %s", strerror(errno), FILENAME);
        close(fd);
        pthread_mutex_unlock(file_mutex);
        return -1;
    }
    close(fd);
    pthread_mutex_unlock(file_mutex);
    return 0;
}
