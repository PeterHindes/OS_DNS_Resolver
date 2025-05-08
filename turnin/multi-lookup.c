#define _POSIX_C_SOURCE 200809L  // Updated for getline and strdup support
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

// hostname_queue is a circular queue of hostnames to be resolved
array hostname_queue;

// logfile queues for requester and resolver logs
array resolver_logfile_queue;
array requester_logfile_queue;

// Mutex and counter to track active file reader threads
pthread_mutex_t file_reader_mutex = PTHREAD_MUTEX_INITIALIZER;
int active_file_readers = 0;

// Thread statistics
typedef struct {
    pthread_t thread_id;
    int files_serviced;
    int hosts_resolved;
    struct timeval start_time;
    struct timeval end_time;
} thread_stats_t;

// read file line by line and insert each line into the hostname queue
void *read_file_line_by_line(void *arg) {
    thread_stats_t *stats = (thread_stats_t *)arg;
    char *filename = (char *)(stats + 1); // Filename is stored after the stats struct
    
    // Record start time and thread ID
    gettimeofday(&stats->start_time, NULL);
    stats->thread_id = pthread_self();
    
    FILE *file = fopen(filename, "r");
    if (!file) {
        fprintf(stderr, "Thread %lx: invalid file %s\n", 
                (unsigned long)stats->thread_id, filename);
        
        pthread_mutex_lock(&file_reader_mutex);
        active_file_readers--;
        if (active_file_readers == 0) {
            array_free(&hostname_queue);
            array_free(&requester_logfile_queue);
        }
        pthread_mutex_unlock(&file_reader_mutex);
        
        // Record end time even for failed files
        gettimeofday(&stats->end_time, NULL);
        return NULL;
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
                fprintf(stderr, "Thread %lx: Failed to put hostname into requester log queue: %s (error: %d)\n", 
                        (unsigned long)stats->thread_id, hostname_for_log, put_result);
                free(hostname_for_log);
            }
        } else {
            fprintf(stderr, "Thread %lx: Failed to allocate memory for hostname log entry\n", 
                    (unsigned long)stats->thread_id);
        }
        
        // Add to resolver queue
        char *hostname_copy = strdup(line);
        if (!hostname_copy) {
            fprintf(stderr, "Thread %lx: Failed to allocate memory for hostname\n", 
                    (unsigned long)stats->thread_id);
            continue;
        }
        
        int put_result = array_put(&hostname_queue, hostname_copy);
        if (put_result != ARRAY_SUCCESS) {
            fprintf(stderr, "Thread %lx: Failed to put hostname into queue: %s (error: %d)\n", 
                    (unsigned long)stats->thread_id, hostname_copy, put_result);
            free(hostname_copy);
        }
    }
    
    free(line);
    fclose(file);
    
    pthread_mutex_lock(&file_reader_mutex);
    active_file_readers--;
    if (active_file_readers == 0) {
        array_free(&hostname_queue);
        array_free(&requester_logfile_queue);
    }
    pthread_mutex_unlock(&file_reader_mutex);
    
    // Record end time
    gettimeofday(&stats->end_time, NULL);
    
    return NULL;
}

// dns resolve function takes a hostname and resolves it to a ip string returned via pointer
void *dns_resolve(const char *hostname, char **ip_string, pthread_t thread_id) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;

    int status = getaddrinfo(hostname, NULL, &hints, &res);
    if (status != 0) {
        fprintf(stderr, "Thread %lx: %s produced getaddrinfo error: %s\n", 
                (unsigned long)thread_id, hostname, gai_strerror(status));
        return NULL;
    }

    // Convert the address to a string
    char ip[INET6_ADDRSTRLEN];
    void *addr;
    if (res->ai_family == AF_INET) { // IPv4
        struct sockaddr_in *ipv4 = (struct sockaddr_in *)res->ai_addr;
        addr = &(ipv4->sin_addr);
        inet_ntop(res->ai_family, addr, ip, sizeof(ip));
    } else { // IPv6
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
    exit(EXIT_SUCCESS);
}

// callback function to resolve hostname and log result
void resolve_then_log(const char *hostname, thread_stats_t *stats) {
    char *ip_string = NULL;
    dns_resolve(hostname, &ip_string, stats->thread_id);
    char *log_message;
    
    if (ip_string) {
        log_message = malloc(strlen(hostname) + strlen(ip_string) + 3);
        if (log_message) {
            sprintf(log_message, "%s, %s", hostname, ip_string);
        } else {
            fprintf(stderr, "Thread %lx: Failed to allocate memory for log message\n", 
                    (unsigned long)stats->thread_id);
        }
        free(ip_string);
    } else {
        log_message = malloc(strlen(hostname) + 20);
        if (log_message) {
            sprintf(log_message, "%s, NOT_RESOLVED", hostname);
        } else {
            fprintf(stderr, "Thread %lx: Failed to allocate memory for log message\n", 
                    (unsigned long)stats->thread_id);
        }
    }

    if (log_message) {
        int put_result = array_put(&resolver_logfile_queue, log_message);
        if (put_result != ARRAY_SUCCESS) {
            fprintf(stderr, "Thread %lx: Failed to put log message into queue: %s (error: %d)\n", 
                    (unsigned long)stats->thread_id, log_message, put_result);
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
    
    // Record start time and thread ID
    gettimeofday(&stats->start_time, NULL);
    stats->thread_id = pthread_self();
    
    while (1) {
        char *hostname = NULL;
        int result = array_get(&hostname_queue, &hostname);
        
        // Handle shutdown condition
        if (result == ARRAY_SHUTDOWN) {
            break;
        }
        
        // Ensure we have a valid hostname
        if (hostname) {
            resolve_then_log(hostname, stats);
            free(hostname);
        } else {
            fprintf(stderr, "Thread %lx: Received NULL hostname from queue\n", 
                    (unsigned long)stats->thread_id);
        }
    }
    
    // Record end time
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
        fprintf(stdout, "Usage: %s <# requesters> <# resolvers> <requester log> <resolver log> [<data file>...]\n", argv[0]);
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
        fprintf(stdout, "Usage: %s <# requesters> <# resolvers> <requester log> <resolver log> [<data file>...]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Check if we have any data files to process
    if (argc < 6) {
        fprintf(stderr, "No data files provided\n");
        fprintf(stdout, "Usage: %s <# requesters> <# resolvers> <requester log> <resolver log> [<data file>...]\n", argv[0]);
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
        fprintf(stdout, "Usage: %s <# requesters> <# resolvers> <requester log> <resolver log> [<data file>...]\n", argv[0]);
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

    // Initialize the queues
    if (array_init(&hostname_queue) != ARRAY_SUCCESS) {
        fprintf(stderr, "Failed to initialize hostname queue\n");
        fclose(requester_logfile);
        fclose(resolver_logfile);
        exit(EXIT_FAILURE);
    }

    if (array_init(&requester_logfile_queue) != ARRAY_SUCCESS) {
        fprintf(stderr, "Failed to initialize requester logfile queue\n");
        fclose(requester_logfile);
        fclose(resolver_logfile);
        array_free(&hostname_queue);
        exit(EXIT_FAILURE);
    }

    if (array_init(&resolver_logfile_queue) != ARRAY_SUCCESS) {
        fprintf(stderr, "Failed to initialize resolver logfile queue\n");
        fclose(requester_logfile);
        fclose(resolver_logfile);
        array_free(&hostname_queue);
        array_free(&requester_logfile_queue);
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
        exit(EXIT_FAILURE);
    }

    // Allocate memory for thread statistics and thread arrays early
    thread_stats_t *resolver_stats = calloc(num_resolvers, sizeof(thread_stats_t));
    pthread_t *resolver_threads = malloc(num_resolvers * sizeof(pthread_t));
    pthread_t *file_threads = malloc(valid_files * sizeof(pthread_t));
    thread_stats_t *file_stats = calloc(valid_files, sizeof(thread_stats_t));
    
    if (!resolver_stats || !resolver_threads || !file_threads || !file_stats) {
        perror("Failed to allocate memory for thread structures");
        fclose(requester_logfile);
        fclose(resolver_logfile);
        array_free(&hostname_queue);
        array_free(&requester_logfile_queue);
        array_free(&resolver_logfile_queue);
        free(resolver_stats);
        free(resolver_threads);
        free(file_threads);
        free(file_stats);
        exit(EXIT_FAILURE);
    }

    // Set the count of active file readers
    pthread_mutex_lock(&file_reader_mutex);
    active_file_readers = valid_files;
    pthread_mutex_unlock(&file_reader_mutex);
    
    // Create a thread for each valid file
    int thread_index = 0;
    for (int i = 5; i < argc; i++) {
        FILE *test_file = fopen(argv[i], "r");
        if (!test_file) {
            continue;  // Skip invalid files
        }
        fclose(test_file);
        
        // Allocate memory for thread args (stats + filename)
        size_t filename_len = strlen(argv[i]) + 1;
        void *arg = malloc(sizeof(thread_stats_t) + filename_len);
        if (!arg) {
            fprintf(stderr, "Failed to allocate memory for thread argument\n");
            active_file_readers--; // Adjust for files we can't process
            continue;
        }
        
        // Copy stats structure and filename
        thread_stats_t *thread_stats = (thread_stats_t *)arg;
        memset(thread_stats, 0, sizeof(thread_stats_t));
        char *filename_ptr = (char *)(thread_stats + 1);
        strcpy(filename_ptr, argv[i]);
        
        // Create thread for this file
        if (pthread_create(&file_threads[thread_index], NULL, read_file_line_by_line, arg) != 0) {
            perror("Failed to create file reader thread");
            free(arg);
            active_file_readers--; // Adjust for threads we can't create
            continue;
        }
        
        file_stats[thread_index] = *thread_stats;
        file_stats[thread_index].thread_id = file_threads[thread_index];
        thread_index++;
    }
    
    // Ensure we have at least one thread to process files
    if (thread_index == 0 && valid_files > 0) {
        fprintf(stderr, "Failed to create any file processing threads despite having valid files\n");
        fclose(requester_logfile);
        fclose(resolver_logfile);
        array_free(&hostname_queue);
        array_free(&requester_logfile_queue);
        array_free(&resolver_logfile_queue);
        free(resolver_stats);
        free(file_threads);
        free(file_stats);
        free(resolver_threads);
        exit(EXIT_FAILURE);
    }

    // Create resolver threads - no need to allocate memory again
    for (int i = 0; i < num_resolvers; i++) {
        thread_stats_t *stats = &resolver_stats[i];
        if (pthread_create(&resolver_threads[i], NULL, resolver_thread, stats) != 0) {
            perror("Failed to create resolver thread");
            fclose(requester_logfile);
            fclose(resolver_logfile);
            array_free(&hostname_queue);
            array_free(&requester_logfile_queue);
            array_free(&resolver_logfile_queue);
            free(resolver_stats);
            free(file_threads);
            free(file_stats);
            free(resolver_threads);
            exit(EXIT_FAILURE);
        }
        stats->thread_id = resolver_threads[i];
    }
    
    // Wait for all file threads to finish
    for (int i = 0; i < valid_files; i++) {
        pthread_join(file_threads[i], NULL);
        double thread_runtime = (file_stats[i].end_time.tv_sec - file_stats[i].start_time.tv_sec) + 
                               ((file_stats[i].end_time.tv_usec - file_stats[i].start_time.tv_usec) / 1000000.0);
        printf("thread %lx serviced %d files in %f seconds\n", 
               (unsigned long)file_stats[i].thread_id, 
               file_stats[i].files_serviced, 
               thread_runtime);
    }
    
    // Wait for all resolver threads to finish
    for (int i = 0; i < num_resolvers; i++) {
        pthread_join(resolver_threads[i], NULL);
        double thread_runtime = (resolver_stats[i].end_time.tv_sec - resolver_stats[i].start_time.tv_sec) + 
                               ((resolver_stats[i].end_time.tv_usec - resolver_stats[i].start_time.tv_usec) / 1000000.0);
        printf("thread %lx resolved %d hosts in %f seconds\n", 
               (unsigned long)resolver_stats[i].thread_id,
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
    free(file_threads);
    free(file_stats);
    free(resolver_threads);
    free(resolver_stats);
    
    // Calculate and print total runtime
    gettimeofday(&end_time, NULL);
    double runtime = (end_time.tv_sec - start_time.tv_sec) + 
                     ((end_time.tv_usec - start_time.tv_usec) / 1000000.0);
    printf("%s: total time is %f seconds\n", argv[0], runtime);
    
    return EXIT_SUCCESS;
}