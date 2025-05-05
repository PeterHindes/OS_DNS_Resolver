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

#define NUM_RESOLVER_THREADS 1

// hostname_queue is a circular queue of hostnames to be resolved
array hostname_queue;

// logfile_queue is a circular queue of log messages to be written to the logfile
array logfile_queue;

// Mutex and counter to track active file reader threads
pthread_mutex_t file_reader_mutex = PTHREAD_MUTEX_INITIALIZER;
int active_file_readers = 0;

/*
 * architecture overview:
 * the main thread spawns a thread for each file which fills the hostname queue
 * the resolver threads take hostnames from the queue and resolve them and generate a log message which is stored in the logfile queue
 * the main thread spawns a single thread that takes log messages from the logfile queue and writes them to the logfile
 * when the file threads are done they rejoin the main thread which then waits for the logfile thread and the resolver threads to all be idle and then shuts down
*/

// read file line by line and insert each line into the hostname queue
void *read_file_line_by_line(void *arg) {
    const char *filename = (const char *)arg;
    FILE *file = fopen(filename, "r");
    if (!file) {
        fprintf(stderr, "invalid file %s\n", filename);
        
        pthread_mutex_lock(&file_reader_mutex);
        active_file_readers--;
        if (active_file_readers == 0) {
            array_free(&hostname_queue);
        }
        pthread_mutex_unlock(&file_reader_mutex);
        
        return NULL;
    }
    
    char *line = NULL;
    size_t len = 0;
    ssize_t read;
    while ((read = getline(&line, &len, file)) != -1) {
        if (read > 0 && line[read - 1] == '\n') {
            line[read - 1] = '\0';
        }
        
        char *hostname_copy = strdup(line);
        if (!hostname_copy) {
            fprintf(stderr, "Failed to allocate memory for hostname\n");
            continue;
        }
        
        if (array_put(&hostname_queue, hostname_copy) != 0) {
            fprintf(stderr, "Failed to put hostname into queue: %s\n", hostname_copy);
            free(hostname_copy);
        }
    }
    
    free(line);
    fclose(file);
    
    pthread_mutex_lock(&file_reader_mutex);
    active_file_readers--;
    if (active_file_readers == 0) {
        array_free(&hostname_queue);
    }
    pthread_mutex_unlock(&file_reader_mutex);
    
    return NULL;
}

// dns resolve function takes a hostname and resolves it to a ip string returned via pointer
void *dns_resolve(const char *hostname, char **ip_string) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;

    int status = getaddrinfo(hostname, NULL, &hints, &res);
    if (status != 0) {
        fprintf(stderr, "%s produced getaddrinfo error: %s\n", hostname, gai_strerror(status));
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
    exit(EXIT_SUCCESS);
}

// callback function to resolve hostname and log result
void resolve_then_log(const char *hostname) {
    char *ip_string = NULL;
    dns_resolve(hostname, &ip_string);
    char *log_message;
    
    if (ip_string) {
        log_message = malloc(strlen(hostname) + strlen(ip_string) + 3);
        if (log_message) {
            sprintf(log_message, "%s, %s", hostname, ip_string);
        } else {
            fprintf(stderr, "Failed to allocate memory for log message\n");
        }
        free(ip_string);
    } else {
        log_message = malloc(strlen(hostname) + 20);
        if (log_message) {
            sprintf(log_message, "%s, NOT_RESOLVED", hostname);
        } else {
            fprintf(stderr, "Failed to allocate memory for log message\n");
        }
    }

    if (log_message) {
        if (array_put(&logfile_queue, log_message) != 0) {
            fprintf(stderr, "Failed to put log message into queue: %s\n", log_message);
            free(log_message);
        }
    }
}

// logfile thread function, it scans the logfile queue and writes the log messages to the logfile
void *logfile_thread_func(void *arg) {
    FILE *logfile = (FILE *)arg;
    char *log_message;
    while (1) {
        if (array_get(&logfile_queue, &log_message) == ARRAY_SHUTDOWN) {
            break;
        }
        write_logfile(logfile, log_message);
        free(log_message);
    }
    return NULL;
}

// threadmain function for resolver (consumer) threads
void *resolver_thread(void *arg) {
    (void)arg;
    
    while (1) {
        char *hostname;
        if (array_get(&hostname_queue, &hostname) == ARRAY_SHUTDOWN) {
            break;
        }
        resolve_then_log(hostname);
        free(hostname);
    }
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
    if (argc < 3) {
        fprintf(stdout, "Usage: %s <log_file> [<data_file>...]\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    
    // Check if each data file exists and is readable before proceeding
    int valid_files = 0;
    for (int i = 2; i < argc; i++) {
        FILE *test_file = fopen(argv[i], "r");
        if (test_file) {
            valid_files++;
            fclose(test_file);
        } else {
            fprintf(stderr, "invalid file %s\n", argv[i]);
        }
    }
    
    // If no valid files were found, print usage and exit
    if (valid_files == 0) {
        fprintf(stdout, "Usage: %s <log_file> [<data_file>...]\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    
    char *log_file = argv[1];
    FILE *logfile = fopen(log_file, "w");
    if (!logfile) {
        perror("Failed to open log file");
        exit(EXIT_FAILURE);
    }

    if (array_init(&hostname_queue) != 0) {
        fprintf(stderr, "Failed to initialize hostname queue\n");
        fclose(logfile);
        exit(EXIT_FAILURE);
    }
    if (array_init(&logfile_queue) != 0) {
        fprintf(stderr, "Failed to initialize logfile queue\n");
        fclose(logfile);
        array_free(&hostname_queue);
        exit(EXIT_FAILURE);
    }

    // Set the initial count of active file readers to the number of valid files
    pthread_mutex_lock(&file_reader_mutex);
    active_file_readers = valid_files;
    pthread_mutex_unlock(&file_reader_mutex);

    // spawn a thread for each file
    pthread_t file_threads[argc - 2];
    for (int i = 2; i < argc; i++) {
        if (pthread_create(&file_threads[i - 2], NULL, read_file_line_by_line, (void *)argv[i]) != 0) {
            perror("Failed to create file thread");
            fclose(logfile);
            array_free(&hostname_queue);
            exit(EXIT_FAILURE);
        }
    }

    // Create resolver threads
    pthread_t threads[NUM_RESOLVER_THREADS];
    for (int i = 0; i < NUM_RESOLVER_THREADS; i++) {
        if (pthread_create(&threads[i], NULL, resolver_thread, NULL) != 0) {
            perror("Failed to create resolver thread");
            fclose(logfile);
            array_free(&hostname_queue);
            exit(EXIT_FAILURE);
        }
    }

    // Create the logfile thread
    pthread_t logfile_thread;
    if (pthread_create(&logfile_thread, NULL, logfile_thread_func, (void *)logfile) != 0) {
        perror("Failed to create logfile thread");
        fclose(logfile);
        array_free(&hostname_queue);
        exit(EXIT_FAILURE);
    }

    // Wait for all file reading threads to finish
    for (int i = 0; i < argc - 2; i++) {
        pthread_join(file_threads[i], NULL);
    }
    
    // Wait for all resolver threads to finish
    for (int i = 0; i < NUM_RESOLVER_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    // Signal the logfile queue to shutdown
    array_free(&logfile_queue);
    
    // Wait for the logfile thread to finish
    pthread_join(logfile_thread, NULL);

    // Close the logfile
    fclose(logfile);
    
    // Calculate and print total runtime
    gettimeofday(&end_time, NULL);
    double runtime = (end_time.tv_sec - start_time.tv_sec) + 
                    ((end_time.tv_usec - start_time.tv_usec) / 1000000.0);
    
    printf("%s: total time is %f seconds\n", argv[0], runtime);
    
    return EXIT_SUCCESS;
}