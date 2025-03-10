#include "array.h"

int array_init(array *s) {
    s->head = 0;
    s->tail = 0;
    s->shutdown = 0;
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->not_empty, NULL);
    pthread_cond_init(&s->not_full, NULL);
    return ARRAY_SUCCESS;
}

int array_put(array *s, char *hostname) {
    // lock the queue
    pthread_mutex_lock(&s->lock);

    // Check if shutdown is requested
    if (s->shutdown) {
        pthread_mutex_unlock(&s->lock);
        return ARRAY_SHUTDOWN;
    }

    // wait until the queue is not full
    while (!s->shutdown && (s->tail + 1) % ARRAY_SIZE == s->head) {
        pthread_cond_wait(&s->not_full, &s->lock);
    }

    // Check again after waking up
    if (s->shutdown) {
        pthread_mutex_unlock(&s->lock);
        return ARRAY_SHUTDOWN;
    }

    // add the element to the queue
    s->hostname[s->tail] = hostname;
    s->tail = (s->tail + 1) % ARRAY_SIZE;

    // signal that the queue is not empty, awakening one consumer
    pthread_cond_signal(&s->not_empty);

    // allow other threads to access the queue
    pthread_mutex_unlock(&s->lock);

    return ARRAY_SUCCESS;
}

int array_get(array *s, char **hostname) {
    // lock the queue
    pthread_mutex_lock(&s->lock);

    // wait until the queue is not empty
    while (!s->shutdown && s->head == s->tail) {
        pthread_cond_wait(&s->not_empty, &s->lock);
    }

    // Check if shutdown was requested
    if (s->shutdown && s->head == s->tail) {
        pthread_mutex_unlock(&s->lock);
        *hostname = NULL;
        return ARRAY_SHUTDOWN;
    }

    // remove the element from the queue
    *hostname = s->hostname[s->head];
    s->head = (s->head + 1) % ARRAY_SIZE;

    // signal that the queue is not full, awakening one producer
    pthread_cond_signal(&s->not_full);

    // allow other threads to access the queue
    pthread_mutex_unlock(&s->lock);

    return ARRAY_SUCCESS;
}

void array_shutdown(array *s) {
    pthread_mutex_lock(&s->lock);
    s->shutdown = 1;
    // Wake up all waiting threads
    pthread_cond_broadcast(&s->not_empty);
    pthread_cond_broadcast(&s->not_full);
    pthread_mutex_unlock(&s->lock);
}

void array_free(array *s) {
    // Make sure we've signaled shutdown
    array_shutdown(s);
    
    // Clean up synchronization objects
    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy(&s->not_empty);
    pthread_cond_destroy(&s->not_full);
}
