# DNS Resolver

This project implements a DNS resolver that resolves domain names to IP addresses. It provides both threaded and non-threaded implementations for performance comparison.

## Overview

The DNS resolver takes a list of hostnames from input files, resolves them to IP addresses, and logs the results. The threaded version uses multiple threads to process inputs in parallel, while the non-threaded version processes inputs sequentially.

## Implementation Details

- **Threaded Version**: Uses separate threads for file reading, DNS resolution, and logging
- **Non-threaded Version**: Processes hostnames sequentially in a single thread
- Both versions use the same DNS resolution logic

## Performance Comparison

The threaded implementation is designed to provide better performance when processing multiple files by:
- Reading multiple files concurrently
- Resolving multiple hostnames simultaneously
- Separating I/O operations from computation

## Usage

```
./dnsthreaded <log_file> [<data_file>...]
./dnssimple <log_file> [<data_file>...]
```

Where:
- `<log_file>`: The output file for resolved hostnames
- `<data_file>...`: One or more files containing hostnames to resolve (one per line)

## Error Handling

- Missing arguments: Terminates with a usage synopsis
- Invalid files: Prints error message and continues with valid files
- No valid files: Prints usage and exits
