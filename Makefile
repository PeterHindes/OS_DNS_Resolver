CC = gcc
CFLAGS = -Wall -Wextra -g -pthread
LDFLAGS = -pthread

all: test

test: array.o test.o
	$(CC) $(LDFLAGS) -o $@ $^

array.o: array.c array.h
	$(CC) $(CFLAGS) -c -o $@ $<

test.o: test.c array.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f test *.o

.PHONY: all clean
