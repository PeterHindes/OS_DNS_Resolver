#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <signal.h>
#include <errno.h>
#include "array.h"

// Global usage message constant
const char* USAGE_MESSAGE = "Usage: %s <# requesters> <# resolvers> <requester log> <resolver log> [<data file>...]\n";

// hostname_queue is a circular queue of hostnames to be resolved
array hostname_queue;

// logfile queues for requester and resolver logs
array resolver_logfile_queue;
array requester_logfile_queue;

// filename queue that requesters consume from
array filename_queue;

// Mutex and counter to track active file reader threads
pthread_mutex_t file_reader_mutex = PTHREAD_MUTEX_INITIALIZER;
int active_file_readers = 0;

// Mutex and counter for sequential thread IDs
pthread_mutex_t thread_id_mutex = PTHREAD_MUTEX_INITIALIZER;
int next_thread_id = 1;

// Thread statistics
typedef struct {
    pthread_t thread_id;
    int seq_id;
    int files_serviced;
    int hosts_resolved;
    struct timeval start_time;
    struct timeval end_time;
} thread_stats_t;

// Thread function for file reading that processes files from the filename queue
void *process_files_from_queue(void *arg) {
    thread_stats_t *stats = (thread_stats_t *)arg;
    
    // Record start time and thread ID
    gettimeofday(&stats->start_time, NULL);
    stats->thread_id = pthread_self();
    
    // Keep processing filenames from the queue
    while (1) {
        char *filename = NULL;
        int result = array_get(&filename_queue, &filename);
        
        // Handle shutdown condition
        if (result == ARRAY_SHUTDOWN) {
            break;
        }
        
        if (!filename) {
            continue;
        }
        
        FILE *file = fopen(filename, "r");
        if (!file) {
            fprintf(stderr, "Thread %d: invalid file %s\n", 
                    stats->seq_id, filename);
            free(filename);
            continue;
        }
        
        // Increment files serviced counter
        stats->files_serviced++;
        
        char *line = NULL;
        size_t len = 0;
        ssize_t read;
        while ((read = getline(&line, &len, file)) != -1) {
            if (read > 0 && line[read - 1] == '\n') {
                line[read - 1] = '\0';
            }
            
            // Skip empty lines
            if (strlen(line) == 0) {
                continue;
            }
            
            // Add to requester log queue
            char *hostname_for_log = strdup(line);
            if (hostname_for_log) {
                int put_result = array_put(&requester_logfile_queue, hostname_for_log);
                if (put_result != ARRAY_SUCCESS) {
                    fprintf(stderr, "Thread %d: Failed to put hostname into requester log queue: %s (error: %d)\n", 
                            stats->seq_id, hostname_for_log, put_result);
                    free(hostname_for_log);
                }
            } else {
                fprintf(stderr, "Thread %d: Failed to allocate memory for hostname log entry\n", 
                        stats->seq_id);
            }
            
            // Add to resolver queue
            char *hostname_copy = strdup(line);
            if (!hostname_copy) {
                fprintf(stderr, "Thread %d: Failed to allocate memory for hostname\n", 
                        stats->seq_id);
                continue;
            }
            
            int put_result = array_put(&hostname_queue, hostname_copy);
            if (put_result != ARRAY_SUCCESS) {
                fprintf(stderr, "Thread %d: Failed to put hostname into queue: %s (error: %d)\n", 
                        stats->seq_id, hostname_copy, put_result);
                free(hostname_copy);
            }
        }
        
        free(line);
        fclose(file);
        free(filename);
    }
    
    pthread_mutex_lock(&file_reader_mutex);
    active_file_readers--;
    if (active_file_readers == 0) {
        array_free(&hostname_queue);
        array_free(&requester_logfile_queue);
    }
    pthread_mutex_unlock(&file_reader_mutex);
    
    gettimeofday(&stats->end_time, NULL);
    
    return NULL;
}

// dns resolve function takes a hostname and resolves it to a ip string returned via pointer
void *dns_resolve(const char *hostname, char **ip_string, int thread_id) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int status = getaddrinfo(hostname, NULL, &hints, &res);
    if (status != 0) {
        fprintf(stderr, "Thread %d: %s produced getaddrinfo error: %s\n", 
                thread_id, hostname, gai_strerror(status));
        return NULL;
    }

    char ip[INET6_ADDRSTRLEN];
    void *addr;
    if (res->ai_family == AF_INET) {
        struct sockaddr_in *ipv4 = (struct sockaddr_in *)res->ai_addr;
        addr = &(ipv4->sin_addr);
        inet_ntop(res->ai_family, addr, ip, sizeof(ip));
    } else {
        struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)res->ai_addr;
        addr = &(ipv6->sin6_addr);
        inet_ntop(res->ai_family, addr, ip, sizeof(ip));
    }

    *ip_string = strdup(ip);
    freeaddrinfo(res);
    return NULL;
}

// write logfile function (file handler, message)
void write_logfile(FILE *file, const char *message) {
    if (file) {
        fprintf(file, "%s\n", message);
        fflush(file);
    }
}

// signal handler for SIGINT
void handle_signal(int sig) {
    printf("\nReceived signal %d, shutting down...\n", sig);
    array_free(&hostname_queue);
    array_free(&requester_logfile_queue);
    array_free(&resolver_logfile_queue);
    array_free(&filename_queue);
    exit(EXIT_SUCCESS);
}

// callback function to resolve hostname and log result
void resolve_then_log(const char *hostname, thread_stats_t *stats) {
    char *ip_string = NULL;
    dns_resolve(hostname, &ip_string, stats->seq_id);
    char *log_message;
    
    if (ip_string) {
        log_message = malloc(strlen(hostname) + strlen(ip_string) + 3);
        if (log_message) {
            sprintf(log_message, "%s, %s", hostname, ip_string);
        } else {
            fprintf(stderr, "Thread %d: Failed to allocate memory for log message\n", 
                    stats->seq_id);
        }
        free(ip_string);
    } else {
        log_message = malloc(strlen(hostname) + 20);
        if (log_message) {
            sprintf(log_message, "%s, NOT_RESOLVED", hostname);
        } else {
            fprintf(stderr, "Thread %d: Failed to allocate memory for log message\n", 
                    stats->seq_id);
        }
    }

    if (log_message) {
        int put_result = array_put(&resolver_logfile_queue, log_message);
        if (put_result != ARRAY_SUCCESS) {
            fprintf(stderr, "Thread %d: Failed to put log message into queue: %s (error: %d)\n", 
                    stats->seq_id, log_message, put_result);
            free(log_message);
        } else {
            stats->hosts_resolved++;
        }
    }
}

// requester logfile thread function
void *requester_logfile_thread_func(void *arg) {
    FILE *logfile = (FILE *)arg;
    char *log_message;
    while (1) {
        if (array_get(&requester_logfile_queue, &log_message) == ARRAY_SHUTDOWN) {
            break;
        }
        write_logfile(logfile, log_message);
        free(log_message);
    }
    return NULL;
}

// resolver logfile thread function
void *resolver_logfile_thread_func(void *arg) {
    FILE *logfile = (FILE *)arg;
    char *log_message;
    while (1) {
        if (array_get(&resolver_logfile_queue, &log_message) == ARRAY_SHUTDOWN) {
            break;
        }
        write_logfile(logfile, log_message);
        free(log_message);
    }
    return NULL;
}

// threadmain function for resolver (consumer) threads
void *resolver_thread(void *arg) {
    thread_stats_t *stats = (thread_stats_t *)arg;
    
    gettimeofday(&stats->start_time, NULL);
    stats->thread_id = pthread_self();
    
    while (1) {
        char *hostname = NULL;
        int result = array_get(&hostname_queue, &hostname);
        
        if (result == ARRAY_SHUTDOWN) {
            break;
        }
        
        if (hostname) {
            resolve_then_log(hostname, stats);
            free(hostname);
        } else {
            fprintf(stderr, "Thread %d: Received NULL hostname from queue\n", 
                    stats->seq_id);
        }
    }
    
    gettimeofday(&stats->end_time, NULL);
    return NULL;
}

int main(int argc, char *argv[]) {
    // Start timing the execution
    struct timeval start_time, end_time;
    gettimeofday(&start_time, NULL);
    
    // Set up signal handlers
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN); // Ignore SIGPIPE to prevent termination on broken pipe

    // Check arguments
    if (argc < 5) {
        fprintf(stdout, USAGE_MESSAGE, argv[0]);
        exit(EXIT_FAILURE);
    }

    // Parse command line arguments
    int num_requesters = atoi(argv[1]);
    int num_resolvers = atoi(argv[2]);
    char *requester_log_file = argv[3];
    char *resolver_log_file = argv[4];

    // Validate numeric arguments
    if (num_requesters <= 0 || num_resolvers <= 0) {
        fprintf(stderr, "Number of requester and resolver threads must be positive integers\n");
        fprintf(stdout, USAGE_MESSAGE, argv[0]);
        exit(EXIT_FAILURE);
    }

    // Check if we have any data files to process
    if (argc < 6) {
        fprintf(stderr, "No data files provided\n");
        fprintf(stdout, USAGE_MESSAGE, argv[0]);
        exit(EXIT_FAILURE);
    }

    // Count valid files
    int valid_files = 0;
    for (int i = 5; i < argc; i++) {
        FILE *test_file = fopen(argv[i], "r");
        if (test_file) {
            fclose(test_file);
            valid_files++;
        } else {
            fprintf(stderr, "invalid file %s\n", argv[i]);
        }
    }

    // If no valid files were found, print usage and exit
    if (valid_files == 0) {
        fprintf(stdout, USAGE_MESSAGE, argv[0]);
        exit(EXIT_FAILURE);
    }

    // Open log files
    FILE *requester_logfile = fopen(requester_log_file, "w");
    if (!requester_logfile) {
        perror("Failed to open requester log file");
        exit(EXIT_FAILURE);
    }

    FILE *resolver_logfile = fopen(resolver_log_file, "w");
    if (!resolver_logfile) {
        perror("Failed to open resolver log file");
        fclose(requester_logfile);
        exit(EXIT_FAILURE);
    }

    // Initialize all queues
    if (array_init(&hostname_queue) != ARRAY_SUCCESS ||
        array_init(&requester_logfile_queue) != ARRAY_SUCCESS ||
        array_init(&resolver_logfile_queue) != ARRAY_SUCCESS ||
        array_init(&filename_queue) != ARRAY_SUCCESS) {
        
        fprintf(stderr, "Failed to initialize queue(s)\n");
        fclose(requester_logfile);
        fclose(resolver_logfile);
        array_free(&hostname_queue);
        array_free(&requester_logfile_queue);
        array_free(&resolver_logfile_queue);
        array_free(&filename_queue);
        exit(EXIT_FAILURE);
    }

    // Create the requester log thread
    pthread_t requester_log_thread;
    if (pthread_create(&requester_log_thread, NULL, requester_logfile_thread_func, requester_logfile) != 0) {
        perror("Failed to create requester log thread");
        fclose(requester_logfile);
        fclose(resolver_logfile);
        array_free(&hostname_queue);
        array_free(&requester_logfile_queue);
        array_free(&resolver_logfile_queue);
        array_free(&filename_queue);
        exit(EXIT_FAILURE);
    }

    // Create the resolver log thread
    pthread_t resolver_log_thread;
    if (pthread_create(&resolver_log_thread, NULL, resolver_logfile_thread_func, resolver_logfile) != 0) {
        perror("Failed to create resolver log thread");
        fclose(requester_logfile);
        fclose(resolver_logfile);
        array_free(&hostname_queue);
        array_free(&requester_logfile_queue);
        array_free(&resolver_logfile_queue);
        array_free(&filename_queue);
        exit(EXIT_FAILURE);
    }

    // Allocate memory for thread arrays
    thread_stats_t *resolver_stats = calloc(num_resolvers, sizeof(thread_stats_t));
    thread_stats_t *requester_stats = calloc(num_requesters, sizeof(thread_stats_t));
    pthread_t *resolver_threads = malloc(num_resolvers * sizeof(pthread_t));
    pthread_t *requester_threads = malloc(num_requesters * sizeof(pthread_t));
    
    if (!resolver_stats || !resolver_threads || !requester_stats || !requester_threads) {
        perror("Failed to allocate memory for thread structures");
        fclose(requester_logfile);
        fclose(resolver_logfile);
        array_free(&hostname_queue);
        array_free(&requester_logfile_queue);
        array_free(&resolver_logfile_queue);
        array_free(&filename_queue);
        free(resolver_stats);
        free(resolver_threads);
        free(requester_stats);
        free(requester_threads);
        exit(EXIT_FAILURE);
    }

    // Set the count of active file readers
    pthread_mutex_lock(&file_reader_mutex);
    active_file_readers = num_requesters;
    pthread_mutex_unlock(&file_reader_mutex);
    
    // Create requester threads - exactly num_requesters threads
    for (int i = 0; i < num_requesters; i++) {
        thread_stats_t *stats = &requester_stats[i];
        
        // Assign a sequential thread ID
        pthread_mutex_lock(&thread_id_mutex);
        stats->seq_id = next_thread_id++;
        pthread_mutex_unlock(&thread_id_mutex);
        
        if (pthread_create(&requester_threads[i], NULL, process_files_from_queue, stats) != 0) {
            perror("Failed to create requester thread");
            pthread_mutex_lock(&file_reader_mutex);
            active_file_readers--;
            pthread_mutex_unlock(&file_reader_mutex);
            continue;
        }
    }
    
    // Now that threads are ready, add filenames to the queue
    for (int i = 5; i < argc; i++) {
        FILE *test_file = fopen(argv[i], "r");
        if (!test_file) {
            continue; // Skip invalid files
        }
        fclose(test_file);
        
        char *filename_copy = strdup(argv[i]);
        if (!filename_copy) {
            fprintf(stderr, "Failed to allocate memory for filename\n");
            continue;
        }
        
        int put_result = array_put(&filename_queue, filename_copy);
        if (put_result != ARRAY_SUCCESS) {
            fprintf(stderr, "Failed to add filename to queue: %s\n", argv[i]);
            free(filename_copy);
        }
    }
    
    // Create resolver threads - exactly num_resolvers threads
    for (int i = 0; i < num_resolvers; i++) {
        thread_stats_t *stats = &resolver_stats[i];
        
        // Assign a sequential thread ID
        pthread_mutex_lock(&thread_id_mutex);
        stats->seq_id = next_thread_id++;
        pthread_mutex_unlock(&thread_id_mutex);
        
        if (pthread_create(&resolver_threads[i], NULL, resolver_thread, stats) != 0) {
            perror("Failed to create resolver thread");
            // Continue with other threads - this is not critical
        }
    }
    
    // Signal that all filenames have been added to the queue
    array_free(&filename_queue);
    
    // Wait for all requester threads to finish
    for (int i = 0; i < num_requesters; i++) {
        pthread_join(requester_threads[i], NULL);
        double thread_runtime = (requester_stats[i].end_time.tv_sec - requester_stats[i].start_time.tv_sec) + 
                               ((requester_stats[i].end_time.tv_usec - requester_stats[i].start_time.tv_usec) / 1000000.0);
        printf("thread %d serviced %d files in %f seconds\n", 
               requester_stats[i].seq_id, 
               requester_stats[i].files_serviced, 
               thread_runtime);
    }
    
    // Wait for all resolver threads to finish
    for (int i = 0; i < num_resolvers; i++) {
        pthread_join(resolver_threads[i], NULL);
        double thread_runtime = (resolver_stats[i].end_time.tv_sec - resolver_stats[i].start_time.tv_sec) + 
                               ((resolver_stats[i].end_time.tv_usec - resolver_stats[i].start_time.tv_usec) / 1000000.0);
        printf("thread %d resolved %d hosts in %f seconds\n", 
               resolver_stats[i].seq_id,
               resolver_stats[i].hosts_resolved,
               thread_runtime);
    }
    
    // Signal the logfile queues to shutdown
    array_free(&requester_logfile_queue);
    array_free(&resolver_logfile_queue);
    
    // Wait for the logfile threads to finish
    pthread_join(requester_log_thread, NULL);
    pthread_join(resolver_log_thread, NULL);
    
    // Close the logfiles
    fclose(requester_logfile);
    fclose(resolver_logfile);
    
    // Free allocated memory
    free(resolver_threads);
    free(resolver_stats);
    free(requester_threads);
    free(requester_stats);
    
    // Calculate and print total runtime
    gettimeofday(&end_time, NULL);
    double runtime = (end_time.tv_sec - start_time.tv_sec) + 
                     ((end_time.tv_usec - start_time.tv_usec) / 1000000.0);
    printf("%s: total time is %f seconds\n", argv[0], runtime);
    
    return EXIT_SUCCESS;
}