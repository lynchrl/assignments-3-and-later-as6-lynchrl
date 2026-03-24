#ifndef HANDLER_H
#define HANDLER_H

#include <pthread.h>
#include "aesdsocket.h"

void *handle_connection(void *arg);
int append_packet(const char *fname, const char *buf, size_t len, pthread_mutex_t *file_mutex);

#endif // HANDLER_H