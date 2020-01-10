#ifndef DB_H_
#define DB_H_

#include <pthread.h>

#define MAXLEN 256

typedef struct node {
    char *name;
    char *value;
    struct node *lchild;
    struct node *rchild;
} node_t;

extern node_t head;

extern void interpret_command(char *command, char *response, int resp_capacity);
extern int db_print(char *filename);
extern void db_cleanup(void);

#endif  // DB_H_