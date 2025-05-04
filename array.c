#include "array.h"
#include <stdio.h>
#include <time.h>
#include <errno.h>

// Helper function to get a timestamp for a timeout
static void get_timeout(struct timespec *ts, int milliseconds) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_nsec += milliseconds * 1000000; // Convert ms to ns
    if (ts->tv_nsec >= 1000000000) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000;
    }
}

int array_init(array *s) {
    s->head = 0;
    s->tail = 0;
    s->shutdown = 0;
    atomic_init(&s->active_getters, 0);
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->not_empty, NULL);
    pthread_cond_init(&s->not_full, NULL);
    pthread_cond_init(&s->all_getters_done, NULL);
    pthread_cond_init(&s->array_emptied, NULL);
    return ARRAY_SUCCESS;
}

int array_put(array *s, char *storeString) {
    // lock the queue
    pthread_mutex_lock(&s->lock);

    // Check if shutdown is requested
    if (s->shutdown) {
        pthread_mutex_unlock(&s->lock);
        return ARRAY_SHUTDOWN;
    }

    // wait until the queue is not full
    while (!s->shutdown && (s->tail + 1) % ARRAY_SIZE == s->head) {
        struct timespec timeout;
        get_timeout(&timeout, 100); // 100ms timeout
        
        pthread_cond_timedwait(&s->not_full, &s->lock, &timeout);
        
        // Check for shutdown after waking up
        if (s->shutdown) {
            pthread_mutex_unlock(&s->lock);
            return ARRAY_SHUTDOWN;
        }
    }

    // add the element to the queue
    s->storeString[s->tail] = storeString;
    s->tail = (s->tail + 1) % ARRAY_SIZE;

    // signal that the queue is not empty, awakening one getter
    pthread_cond_signal(&s->not_empty);

    // allow other threads to access the queue
    pthread_mutex_unlock(&s->lock);

    return ARRAY_SUCCESS;
}

int array_get(array *s, char **storeString) {
    // add this getter to the active count
    atomic_fetch_add(&s->active_getters, 1);
    
    // lock the queue
    pthread_mutex_lock(&s->lock);

    // wait until the queue is not empty
    while (!s->shutdown && s->head == s->tail) {
        struct timespec timeout;
        get_timeout(&timeout, 100); // 100ms timeout
        
        pthread_cond_timedwait(&s->not_empty, &s->lock, &timeout);
        
        // Check for shutdown after waking up
        if (s->shutdown && s->head == s->tail) {
            // Mark this getter as no longer active
            int remaining = atomic_fetch_sub(&s->active_getters, 1) - 1;
            
            // If this was the last active getter, signal that all are done
            if (remaining == 0) {
                pthread_cond_signal(&s->all_getters_done);
            }
            
            pthread_mutex_unlock(&s->lock);
            *storeString = NULL;
            return ARRAY_SHUTDOWN;
        }
    }

    // Check again if shutdown was requested with an empty queue
    if (s->shutdown && s->head == s->tail) {
        // Mark this getter as no longer active
        int remaining = atomic_fetch_sub(&s->active_getters, 1) - 1;
        
        // If this was the last active getter, signal that all are done
        if (remaining == 0) {
            pthread_cond_signal(&s->all_getters_done);
        }
        
        pthread_mutex_unlock(&s->lock);
        *storeString = NULL;
        return ARRAY_SHUTDOWN;
    }

    // remove the element from the queue
    *storeString = s->storeString[s->head];
    s->head = (s->head + 1) % ARRAY_SIZE;

    // Check if the array is now empty and signal if it is
    if (s->head == s->tail) {
        pthread_cond_signal(&s->array_emptied);
    }

    // signal that the queue is not full, awakening one putter if any are sleeping
    pthread_cond_signal(&s->not_full);

    // allow other threads to access the queue
    pthread_mutex_unlock(&s->lock);

    // reduce the counter of active getters before returning
    atomic_fetch_sub(&s->active_getters, 1);
    
    return ARRAY_SUCCESS;
}

void array_free(array *s) {
    // First, lock the queue
    pthread_mutex_lock(&s->lock);
    
    // Set the shutdown flag
    s->shutdown = 1;
    
    // Wake up all waiting threads
    pthread_cond_broadcast(&s->not_empty);
    // pthread_cond_broadcast(&s->not_full); // Shouldn't produce any more items the queue could be full
    
    // Wait for all getters to finish what they're doing
    while (atomic_load(&s->active_getters) > 0) {
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1;  // 1 second timeout for feedback
        
        printf("Waiting for %d active getters to finish...\n", 
               atomic_load(&s->active_getters));
        
        // Wait for all_getters_done signal with timeout for progress reporting
        int result = pthread_cond_timedwait(&s->all_getters_done, &s->lock, &timeout);
        if (result == 0) {
            // Signal received, all getters done
            break;
        }
    }
    
    printf("All getters have completed. Shutdown complete.\n");
    pthread_mutex_unlock(&s->lock);
    
    // Clean up synchronization objects
    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy(&s->not_empty);
    pthread_cond_destroy(&s->not_full);
    pthread_cond_destroy(&s->all_getters_done);
    pthread_cond_destroy(&s->array_emptied);
}
