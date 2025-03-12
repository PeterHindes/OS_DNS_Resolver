#include "array.h"
#include <stdio.h>
#include <time.h>
#include <errno.h>

// Helper function to get a timestamp for a timeout in the near future
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
    atomic_init(&s->active_consumers, 0);
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->not_empty, NULL);
    pthread_cond_init(&s->not_full, NULL);
    pthread_cond_init(&s->all_consumers_done, NULL);
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

    // wait until the queue is not full, with periodic timeout
    while (!s->shutdown && (s->tail + 1) % ARRAY_SIZE == s->head) {
        struct timespec timeout;
        get_timeout(&timeout, 100); // 100ms timeout
        
        int ret = pthread_cond_timedwait(&s->not_full, &s->lock, &timeout);
        
        // Check for shutdown after waking up, regardless of reason
        if (s->shutdown) {
            pthread_mutex_unlock(&s->lock);
            return ARRAY_SHUTDOWN;
        }
        
        // For timeouts, we just loop back and try again
        if (ret == ETIMEDOUT) {
            continue;
        }
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
    // Track this consumer as active
    atomic_fetch_add(&s->active_consumers, 1);
    
    // lock the queue
    pthread_mutex_lock(&s->lock);

    // wait until the queue is not empty, with periodic timeout
    while (!s->shutdown && s->head == s->tail) {
        struct timespec timeout;
        get_timeout(&timeout, 100); // 100ms timeout
        
        int ret = pthread_cond_timedwait(&s->not_empty, &s->lock, &timeout);
        
        // Check for shutdown after waking up, regardless of reason
        if (s->shutdown && s->head == s->tail) {
            // Mark this consumer as no longer active
            int remaining = atomic_fetch_sub(&s->active_consumers, 1) - 1;
            
            // If this was the last active consumer, signal that all are done
            if (remaining == 0) {
                pthread_cond_signal(&s->all_consumers_done);
            }
            
            pthread_mutex_unlock(&s->lock);
            *hostname = NULL;
            return ARRAY_SHUTDOWN;
        }
        
        // For timeouts, we just loop back and try again
        if (ret == ETIMEDOUT) {
            continue;
        }
    }

    // Check again if shutdown was requested with an empty queue
    if (s->shutdown && s->head == s->tail) {
        // Mark this consumer as no longer active
        int remaining = atomic_fetch_sub(&s->active_consumers, 1) - 1;
        
        // If this was the last active consumer, signal that all are done
        if (remaining == 0) {
            pthread_cond_signal(&s->all_consumers_done);
        }
        
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

    // reduce the number of active consumers before returning
    atomic_fetch_sub(&s->active_consumers, 1);
    
    return ARRAY_SUCCESS;
}

void array_free(array *s) {
    // First, lock the queue
    pthread_mutex_lock(&s->lock);
    
    // Set the shutdown flag
    s->shutdown = 1;
    
    // Wake up all waiting threads
    pthread_cond_broadcast(&s->not_empty);
    pthread_cond_broadcast(&s->not_full);
    
    // Wait for all consumers to finish what they're doing
    while (atomic_load(&s->active_consumers) > 0) {
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1;  // 1 second timeout for feedback
        
        printf("Waiting for %d active consumers to finish...\n", 
               atomic_load(&s->active_consumers));
        
        // Wait for all_consumers_done signal with timeout for progress reporting
        int result = pthread_cond_timedwait(&s->all_consumers_done, &s->lock, &timeout);
        if (result == 0) {
            // Signal received, all consumers done
            break;
        }
    }
    
    printf("All consumers have completed. Shutdown complete.\n");
    pthread_mutex_unlock(&s->lock);
    
    // Clean up synchronization objects
    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy(&s->not_empty);
    pthread_cond_destroy(&s->not_full);
    pthread_cond_destroy(&s->all_consumers_done);
}
