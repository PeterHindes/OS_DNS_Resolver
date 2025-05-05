CC = gcc
CFLAGS = -Wall -Wextra -g -pthread
LDFLAGS = -pthread

all: test.run dnssimple.run dnsthreaded.run

test.run: array.o test.o
	$(CC) $(LDFLAGS) -o $@ $^

dnssimple.run: dnssimple.o
	$(CC) $(LDFLAGS) -o $@ $^

dnsthreaded.run: dnsthreaded.o array.o
	$(CC) $(LDFLAGS) -o $@ $^

dnsthreaded.o: dnsthreaded.c array.h
	$(CC) $(CFLAGS) -c -o $@ $<

array.o: array.c array.h
	$(CC) $(CFLAGS) -c -o $@ $<

test.o: test.c array.h
	$(CC) $(CFLAGS) -c -o $@ $<

dnssimple.o: dnssimple.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f test dnsthreaded dnssimple *.o *.run

.PHONY: all clean
