#ifndef COMM_H_
#define COMM_H_

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <ctype.h>
#define BUFLEN 256
#define handle_error_en(en, msg) \
    do {                         \
        errno = en;              \
        perror(msg);             \
        exit(EXIT_FAILURE);      \
    } while (0)

pthread_t start_listener(int port, void (*server_func)(FILE *));
extern void comm_shutdown(FILE *cxstr);
extern int comm_serve(FILE *cxstr, char *resp, char *cmd);

#endif  // COMM_H_
