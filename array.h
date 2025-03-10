#ifndef ARRAY_H
#define ARRAY_H

#include <pthread.h>
#include <string.h>
#include <stdlib.h>

#define ARRAY_SIZE 8
#define MAX_NAME_LENGTH 256

// Return codes
#define ARRAY_SUCCESS 0
#define ARRAY_SHUTDOWN 1

// circular queue
typedef struct {
    char *hostname[ARRAY_SIZE];
    int head;
    int tail;
    // int count; // not needed just use (tail-head)%ARRAY_SIZE
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    int shutdown;  // Shutdown flag
} array;

int  array_init(array *s);                   // initialize the array
int  array_put (array *s, char *hostname);   // place element into the array, block when full
int  array_get (array *s, char **hostname);  // remove element from the array, block when empty
void array_free(array *s);                   // free the array's resources
void array_shutdown(array *s);               // signal threads to shutdown

#endif // ARRAY_H