#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <signal.h>
#include <errno.h>
#include "array.h"

#define MAX_LINE_LENGTH 256
#define NUM_RESOLVER_THREADS 8  // Number of threads to use for DNS resolution

// Structure to hold resolution results
typedef struct {
    char hostname[MAX_NAME_LENGTH];
    char ip_address[INET6_ADDRSTRLEN];  // Large enough for IPv4 or IPv6
    int resolved;                       // 1 if successful, 0 if failed
    double time_taken;                  // Time taken in seconds
} resolution_result;

// Shared array for passing hostnames to resolver threads
array hostname_queue;

// Array to store results
resolution_result *results = NULL;
int result_count = 0;
pthread_mutex_t results_mutex = PTHREAD_MUTEX_INITIALIZER;

// Add a result to the results array
void add_result(const char *hostname, const char *ip, int resolved, double time_taken) {
    pthread_mutex_lock(&results_mutex);
    
    // Resize results array
    results = realloc(results, sizeof(resolution_result) * (result_count + 1));
    if (!results) {
        perror("Failed to allocate memory for results");
        exit(EXIT_FAILURE);
    }
    
    // Add new result
    strncpy(results[result_count].hostname, hostname, MAX_NAME_LENGTH - 1);
    results[result_count].hostname[MAX_NAME_LENGTH - 1] = '\0';
    
    if (ip) {
        strncpy(results[result_count].ip_address, ip, INET6_ADDRSTRLEN - 1);
        results[result_count].ip_address[INET6_ADDRSTRLEN - 1] = '\0';
    } else {
        results[result_count].ip_address[0] = '\0';
    }
    
    results[result_count].resolved = resolved;
    results[result_count].time_taken = time_taken;
    
    result_count++;
    pthread_mutex_unlock(&results_mutex);
}

// Resolver thread function
void *resolver_thread(void *arg) {
    (void)arg;  // Mark parameter as used to silence warning
    
    while (1) {
        char *hostname;
        
        // Get a hostname from the queue
        int ret = array_get(&hostname_queue, &hostname);
        if (ret == ARRAY_SHUTDOWN || !hostname) {
            break;
        }
        
        struct timeval start_time, end_time;
        gettimeofday(&start_time, NULL);
        
        // Resolve the hostname
        struct addrinfo hints, *res;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_UNSPEC;     // IPv4 or IPv6
        hints.ai_socktype = SOCK_STREAM; // TCP
        
        int status = getaddrinfo(hostname, NULL, &hints, &res);
        
        gettimeofday(&end_time, NULL);
        double time_taken = (end_time.tv_sec - start_time.tv_sec) + 
                           (end_time.tv_usec - start_time.tv_usec) / 1000000.0;
        
        if (status != 0) {
            // Resolution failed
            printf("%-30s : Failed to resolve (%s)\n", 
                  hostname, gai_strerror(status));
            add_result(hostname, NULL, 0, time_taken);
        } else {
            // Get the first IP address
            void *addr;
            char ipstr[INET6_ADDRSTRLEN];
            
            if (res->ai_family == AF_INET) { // IPv4
                struct sockaddr_in *ipv4 = (struct sockaddr_in *)res->ai_addr;
                addr = &(ipv4->sin_addr);
            } else { // IPv6
                struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)res->ai_addr;
                addr = &(ipv6->sin6_addr);
            }
            
            // Convert the IP to a string
            inet_ntop(res->ai_family, addr, ipstr, sizeof ipstr);
            
            printf("%-30s : %s (%.3f sec)\n", hostname, ipstr, time_taken);
            add_result(hostname, ipstr, 1, time_taken);
            
            freeaddrinfo(res);
        }
        
        free(hostname);
    }
    
    return NULL;
}

// Print usage information
void print_usage(const char *program_name) {
    printf("Usage: %s [file...]\n", program_name);
    printf("Resolves hostnames to IP addresses.\n\n");
    printf("If file is provided, reads hostnames from the file(s).\n");
    printf("If no file is provided, reads hostnames from standard input.\n");
    printf("Each hostname should be on a separate line.\n");
}

// Signal handler for graceful termination
void handle_signal(int sig) {
    printf("\nReceived signal %d, shutting down...\n", sig);
    array_free(&hostname_queue);
    exit(EXIT_SUCCESS);
}

int main(int argc, char *argv[]) {
    // Set up signal handlers
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    
    // Initialize the hostname queue
    array_init(&hostname_queue);
    
    // Check command-line arguments
    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        print_usage(argv[0]);
        return EXIT_SUCCESS;
    }
    
    // Create resolver threads
    pthread_t threads[NUM_RESOLVER_THREADS];
    for (int i = 0; i < NUM_RESOLVER_THREADS; i++) {
        if (pthread_create(&threads[i], NULL, resolver_thread, NULL) != 0) {
            perror("Failed to create resolver thread");
            array_free(&hostname_queue);
            return EXIT_FAILURE;
        }
    }
    
    // Process input files or stdin
    FILE *input = stdin;
    int total_hostnames = 0;
    
    printf("Resolving hostnames...\n");
    
    if (argc > 1) {
        // Process each file provided on the command line
        for (int i = 1; i < argc; i++) {
            input = fopen(argv[i], "r");
            if (!input) {
                fprintf(stderr, "Error opening file %s: %s\n", 
                       argv[i], strerror(errno));
                continue;
            }
            
            char line[MAX_LINE_LENGTH];
            while (fgets(line, sizeof(line), input)) {
                // Remove newline character
                size_t len = strlen(line);
                if (len > 0 && line[len-1] == '\n') {
                    line[len-1] = '\0';
                }
                
                // Skip empty lines
                if (strlen(line) == 0) {
                    continue;
                }
                
                // Allocate memory for hostname and add to queue
                char *hostname = strdup(line);
                if (!hostname) {
                    perror("Failed to allocate memory for hostname");
                    continue;
                }
                
                array_put(&hostname_queue, hostname);
                total_hostnames++;
            }
            
            fclose(input);
        }
    } else {
        // Read from stdin
        printf("Enter hostnames (one per line, Ctrl-D to finish):\n");
        
        char line[MAX_LINE_LENGTH];
        while (fgets(line, sizeof(line), stdin)) {
            // Remove newline character
            size_t len = strlen(line);
            if (len > 0 && line[len-1] == '\n') {
                line[len-1] = '\0';
            }
            
            // Skip empty lines
            if (strlen(line) == 0) {
                continue;
            }
            
            // Allocate memory for hostname and add to queue
            char *hostname = strdup(line);
            if (!hostname) {
                perror("Failed to allocate memory for hostname");
                continue;
            }
            
            array_put(&hostname_queue, hostname);
            total_hostnames++;
        }
    }
    
    printf("\nSubmitted %d hostnames for resolution.\n", total_hostnames);
    
    // Wait for all resolutions to complete
    array_free(&hostname_queue);
    
    // Wait for resolver threads to finish
    for (int i = 0; i < NUM_RESOLVER_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    
    // Print summary
    int success_count = 0;
    double total_time = 0.0;
    double min_time = 999999.0;
    double max_time = 0.0;
    
    for (int i = 0; i < result_count; i++) {
        if (results[i].resolved) {
            success_count++;
            total_time += results[i].time_taken;
            
            if (results[i].time_taken < min_time) {
                min_time = results[i].time_taken;
            }
            if (results[i].time_taken > max_time) {
                max_time = results[i].time_taken;
            }
        }
    }
    
    printf("\nResolution summary:\n");
    printf("  Total hostnames:  %d\n", total_hostnames);
    printf("  Successful:       %d (%.1f%%)\n", 
           success_count, (float)success_count / total_hostnames * 100);
    printf("  Failed:           %d (%.1f%%)\n", 
           total_hostnames - success_count, 
           (float)(total_hostnames - success_count) / total_hostnames * 100);
    
    if (success_count > 0) {
        printf("  Average time:     %.3f seconds\n", total_time / success_count);
        printf("  Minimum time:     %.3f seconds\n", min_time);
        printf("  Maximum time:     %.3f seconds\n", max_time);
    }
    
    // Clean up
    free(results);
    pthread_mutex_destroy(&results_mutex);
    
    return EXIT_SUCCESS;
}
