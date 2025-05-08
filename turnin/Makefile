CFLAGS = -Wextra -Wall -g -std=gnu99
MAIN = multi-lookup
LDFLAGS = -lpthread
SRCS = multi-lookup.c array.c
HDRS = array.h
OBJS = $(SRCS:.c=.o)

all: $(MAIN)

$(MAIN): $(OBJS)
	cc $(CFLAGS) -o $(MAIN) $(OBJS) $(LDFLAGS)

%.o: %.c $(HDRS)
	cc $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o $(MAIN)

SUBMITFILES = $(SRCS) $(HDRS) Makefile
submit: 
	@read -r -p "Enter your identikey username: " username; \
	echo; echo Bundling the following files for submission; \
	tar --transform "s|^|PA6-$$username/|" -cvf PA6-$$username.txt $(SUBMITFILES); \
	echo; echo Please upload the file PA6-$$username.txt to Canvas to complete your submission; echo

.PHONY: all clean submit