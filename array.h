#ifndef ARRAY_H
#define ARRAY_H

#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>

#define ARRAY_SIZE 8
#define MAX_NAME_LENGTH 256

// Return codes
#define ARRAY_SUCCESS 0
#define ARRAY_SHUTDOWN 1

// circular queue
typedef struct {
    char *storeString[ARRAY_SIZE];
    int head;
    int tail;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    pthread_cond_t all_getters_done;  // Signals when all getters are inactive
    pthread_cond_t array_emptied;     // Signals when the array becomes empty
    int shutdown;                       // Shutdown flag
    atomic_int active_getters;        // Count of active getters
} array;

int  array_init(array *s);                   // initialize the array
int  array_put (array *s, char *storeString);   // place element into the array, block when full
int  array_get (array *s, char **storeString);  // remove element from the array, block when empty
void array_free(array *s);                   // free resources and shutdown the array

#endif // ARRAY_H