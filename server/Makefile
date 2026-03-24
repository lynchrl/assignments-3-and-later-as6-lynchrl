# Makefile for aesdsocket server. Use standard targets for compiling
# and standard variables for compiler and flags. Use the -g flag to include
# debugging symbols in the executable.

CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -g -Wall -Werror
LDFLAGS ?= -lpthread

all: aesdsocket

aesdsocket: aesdsocket.o handler.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

aesdsocket.o: aesdsocket.c aesdsocket.h handler.h
	$(CC) $(CFLAGS) $(LDFLAGS) -c $<

handler.o: handler.c handler.h
	$(CC) $(CFLAGS) $(LDFLAGS) -c $<

clean:
	rm -f aesdsocket aesdsocket.o handler.o