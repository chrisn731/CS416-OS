CC = gcc
CFLAGS = -g -c
AR = ar -rc
RANLIB = ranlib

SCHED = RR
TSLICE=5 ##timeslice variable

all: rpthread.a

rpthread.a: rpthread.o
	$(AR) librpthread.a rpthread.o
	$(RANLIB) librpthread.a

rpthread.o: rpthread.h

ifeq ($(SCHED), RR)
	$(CC) -pthread $(CFLAGS) rpthread.c -DTIMESLICE $(TSLICE)
else ifeq ($(SCHED), MLFQ)
	$(CC) -pthread $(CFLAGS) rpthread.c -DMLFQ -DTIMESLICE $(TSLICE)
else
	echo "no such scheduling algorithm"
endif

clean:
	rm -rf testfile *.o *.a
