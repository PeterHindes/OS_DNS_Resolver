#define _POSIX_C_SOURCE 200809L  // For getline and strdup support
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/time.h>
#include <signal.h>
#include <errno.h>

// dns resolve function takes a hostname and resolves it to a IP address
char* dns_resolve(const char *hostname) {
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
    char *ip_string = malloc(INET6_ADDRSTRLEN);
    if (!ip_string) {
        freeaddrinfo(res);
        return NULL;
    }
    
    void *addr;
    if (res->ai_family == AF_INET) { // IPv4
        struct sockaddr_in *ipv4 = (struct sockaddr_in *)res->ai_addr;
        addr = &(ipv4->sin_addr);
        inet_ntop(res->ai_family, addr, ip_string, INET6_ADDRSTRLEN);
    } else { // IPv6
        struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)res->ai_addr;
        addr = &(ipv6->sin6_addr);
        inet_ntop(res->ai_family, addr, ip_string, INET6_ADDRSTRLEN);
    }

    freeaddrinfo(res);
    return ip_string;
}

// Process a single file
int process_file(const char *filename, FILE *logfile) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        fprintf(stderr, "invalid file %s\n", filename);
        return -1;
    }
    
    char *line = NULL;
    size_t len = 0;
    ssize_t read;
    
    while ((read = getline(&line, &len, file)) != -1) {
        // Remove newline character
        if (read > 0 && line[read - 1] == '\n') {
            line[read - 1] = '\0';
        }
        
        // Resolve the hostname
        char *ip_string = dns_resolve(line);
        
        // Log the result
        if (ip_string) {
            fprintf(logfile, "%s, %s\n", line, ip_string);
            free(ip_string);
        } else {
            fprintf(logfile, "%s, NOT_RESOLVED\n", line);
        }
        
        fflush(logfile);
    }
    
    free(line);
    fclose(file);
    return 0;
}

// Signal handler
void handle_signal(int sig) {
    printf("\nReceived signal %d, shutting down...\n", sig);
    exit(EXIT_SUCCESS);
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
    
    // Open log file
    char *log_file = argv[1];
    FILE *logfile = fopen(log_file, "w");
    if (!logfile) {
        perror("Failed to open log file");
        exit(EXIT_FAILURE);
    }

    // Process each file sequentially
    for (int i = 2; i < argc; i++) {
        process_file(argv[i], logfile);
    }

    // Close the logfile
    fclose(logfile);
    
    // Calculate and print total runtime
    gettimeofday(&end_time, NULL);
    double runtime = (end_time.tv_sec - start_time.tv_sec) + 
                    ((end_time.tv_usec - start_time.tv_usec) / 1000000.0);
    
    printf("%s: total time is %f seconds\n", argv[0], runtime);
    
    return EXIT_SUCCESS;
}
