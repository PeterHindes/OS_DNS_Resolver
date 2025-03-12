#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <signal.h>
#include <sys/resource.h>
#include <errno.h>
#include "array.h"
#include <stdatomic.h>

#define NUM_PRODUCERS 100
#define NUM_CONSUMERS 10
#define ITEMS_PER_PRODUCER 2000
#define TOTAL_ITEMS (NUM_PRODUCERS * ITEMS_PER_PRODUCER)
#define TEST_TIMEOUT_SECONDS 60
#define PROGRESS_UPDATE_INTERVAL 500

// Shared array
array queue;

// Tracking variables for verification
pthread_mutex_t count_mutex = PTHREAD_MUTEX_INITIALIZER;
int items_produced = 0;
int items_consumed = 0;
int *consumed_items;
time_t start_time;

// Flag to track if all consumers should terminate
volatile int force_shutdown = 0;

// Timeout handler
void timeout_handler(int sig) {
    (void)sig; // Mark parameter as used to silence warning
    time_t end_time = time(NULL);
    printf("\n\nTest timed out after %ld seconds!\n", end_time - start_time);
    printf("Items produced: %d, Items consumed: %d\n", items_produced, items_consumed);
    
    // Signal shutdown to release waiting threads
    array_free(&queue);
    force_shutdown = 1;
    exit(EXIT_FAILURE);
}

// Producer thread function
void *producer(void *arg) {
    int id = *(int *)arg;
    int start = id * ITEMS_PER_PRODUCER;
    int end = start + ITEMS_PER_PRODUCER;
    
    for (int i = start; i < end; i++) {
        char *hostname = malloc(MAX_NAME_LENGTH);
        if (!hostname) {
            perror("Failed to allocate memory");
            exit(EXIT_FAILURE);
        }
        
        sprintf(hostname, "host-%d", i);
        
        // Add hostname to array, break if shutdown requested
        int ret = array_put(&queue, hostname);
        if (ret == ARRAY_SHUTDOWN) {
            free(hostname);
            break;
        }
        
        // Update production count
        pthread_mutex_lock(&count_mutex);
        items_produced++;
        pthread_mutex_unlock(&count_mutex);
    }
    
    return NULL;
}

// Consumer thread function
void *consumer(void *arg) {
    (void)arg; // Mark parameter as used to silence warning
    
    while (1) {
        char *hostname;
        
        // Get hostname from array
        int ret = array_get(&queue, &hostname);
        
        // Check if shutdown was requested
        if (ret == ARRAY_SHUTDOWN || force_shutdown) {
            break;
        }
        
        if (hostname == NULL) {
            continue;
        }
        
        // Extract the item number from the hostname
        int item_num;
        if (sscanf(hostname, "host-%d", &item_num) == 1) {
            // Mark this item as consumed
            pthread_mutex_lock(&count_mutex);
            consumed_items[item_num] = 1;
            items_consumed++;
            
            // Update progress periodically
            if (items_consumed % PROGRESS_UPDATE_INTERVAL == 0 || 
                items_consumed == TOTAL_ITEMS) {
                printf("\rProgress: %d/%d items processed (%.1f%%)", 
                       items_consumed, TOTAL_ITEMS, 
                       (float)items_consumed / TOTAL_ITEMS * 100);
                fflush(stdout);
            }
            
            // Check if we're done with all items
            if (items_consumed >= TOTAL_ITEMS) {
                pthread_mutex_unlock(&count_mutex);
                free(hostname);
                break;  // Exit the loop when all work is done
            }
            
            pthread_mutex_unlock(&count_mutex);
        }
        
        free(hostname);
    }
    
    return NULL;
}

void increase_resource_limits() {
    struct rlimit rlim;
    
    if (getrlimit(RLIMIT_NOFILE, &rlim) == 0) {
        rlim.rlim_cur = rlim.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rlim);
    }
    
    if (getrlimit(RLIMIT_STACK, &rlim) == 0) {
        rlim.rlim_cur = rlim.rlim_max;
        setrlimit(RLIMIT_STACK, &rlim);
    }
}

int main() {
    start_time = time(NULL);
    
    increase_resource_limits();
    
    pthread_t *producer_threads = malloc(NUM_PRODUCERS * sizeof(pthread_t));
    pthread_t *consumer_threads = malloc(NUM_CONSUMERS * sizeof(pthread_t));
    int *producer_ids = malloc(NUM_PRODUCERS * sizeof(int));
    
    if (!producer_threads || !consumer_threads || !producer_ids) {
        perror("Failed to allocate thread arrays");
        exit(EXIT_FAILURE);
    }
    
    // Set up timeout
    signal(SIGALRM, timeout_handler);
    alarm(TEST_TIMEOUT_SECONDS);
    
    // Initialize the consumed_items tracking array
    consumed_items = calloc(TOTAL_ITEMS, sizeof(int));
    if (!consumed_items) {
        perror("Failed to allocate tracking array");
        return EXIT_FAILURE;
    }
    
    // Initialize the array
    array_init(&queue);
    
    printf("Starting thread safety test with %d producers and %d consumers...\n", 
           NUM_PRODUCERS, NUM_CONSUMERS);
    printf("Processing a total of %d items...\n", TOTAL_ITEMS);
    
    // Create producer threads
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        producer_ids[i] = i;
        if (pthread_create(&producer_threads[i], NULL, producer, &producer_ids[i]) != 0) {
            perror("Failed to create producer thread");
            array_free(&queue); // Changed from array_shutdown
            return EXIT_FAILURE;
        }
    }
    
    printf("All producer threads created successfully.\n");
    
    // Create consumer threads
    for (int i = 0; i < NUM_CONSUMERS; i++) {
        if (pthread_create(&consumer_threads[i], NULL, consumer, NULL) != 0) {
            perror("Failed to create consumer thread");
            array_free(&queue); // Changed from array_shutdown
            return EXIT_FAILURE;
        }
    }
    
    printf("All consumer threads created successfully.\n");
    printf("Processing items...\n");
    
    // Wait for producer threads to finish
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        pthread_join(producer_threads[i], NULL);
    }
    
    printf("\nAll producers finished with %d items produced. Waiting for consumers...\n", 
           items_produced);
    
    // Monitor progress until all items are consumed
    while (items_consumed < items_produced) {
        printf("\rWaiting for consumers: %d/%d items processed (%.1f%%)", 
               items_consumed, items_produced, 
               (float)items_consumed / items_produced * 100);
        fflush(stdout);
        sleep(1);  // Simple periodic check is enough now
    }
    
    printf("\nAll %d items have been consumed. Initiating shutdown...\n", 
           items_consumed);
    
    // Replace array_shutdown with array_free - this will now handle both shutdown and cleanup
    array_free(&queue);
    
    // Join all consumer threads
    for (int i = 0; i < NUM_CONSUMERS; i++) {
        pthread_join(consumer_threads[i], NULL);
    }
    
    // Cancel the timeout
    alarm(0);
    
    time_t end_time = time(NULL);
    printf("\n\nAll threads completed in %ld seconds. Verifying results...\n", 
           end_time - start_time);
    
    // Verify all items were consumed exactly once
    int error_count = 0;
    for (int i = 0; i < TOTAL_ITEMS; i++) {
        if (consumed_items[i] != 1) {
            printf("Error: Item %d was consumed %d times\n", i, consumed_items[i]);
            error_count++;
            // Limit the number of error messages printed
            if (error_count >= 20) {
                printf("Too many errors, stopping error reporting...\n");
                break;
            }
        }
    }
    
    if (error_count == 0) {
        printf("All %d items were consumed exactly once. Test PASSED!\n", TOTAL_ITEMS);
    } else {
        printf("Test FAILED! %d out of %d items were not consumed correctly.\n", 
               error_count, TOTAL_ITEMS);
    }
    
    // Clean up
    free(consumed_items);
    free(producer_threads);
    free(consumer_threads);
    free(producer_ids);
    
    return (error_count == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
