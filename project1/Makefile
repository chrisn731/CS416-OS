CC = gcc

all: signal syscall threads

signal: signal.c
	$(CC) -o $@ $^

syscall: syscall.c
	$(CC) -o $@ $^

threads: threads.c
	$(CC) -pthread -o $@ $^

clean:
	rm -f signal syscall threads
