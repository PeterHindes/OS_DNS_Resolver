CC = gcc
CFLAGS = -Wall -Wextra -g -pthread
LDFLAGS = -pthread

all: test.run dnsresolve.run dnsnew.run

test.run: array.o test.o
	$(CC) $(LDFLAGS) -o $@ $^

dnsresolve.run: dnsresolve.o array.o
	$(CC) $(LDFLAGS) -o $@ $^

dnsnew.run: dnsnew.o array.o
	$(CC) $(LDFLAGS) -o $@ $^

dnsnew.o: dnsnew.c array.h
	$(CC) $(CFLAGS) -c -o $@ $<

array.o: array.c array.h
	$(CC) $(CFLAGS) -c -o $@ $<

test.o: test.c array.h
	$(CC) $(CFLAGS) -c -o $@ $<

dnsresolve.o: dnsresolve.c array.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f test dnsnew dnsresolve *.o

.PHONY: all clean
