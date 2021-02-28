CC = gcc
CFLAGS = -g -c
CFLAGS += -Wall
CFLAGS += -Wextra
CFLAGS += -Waggregate-return
CFLAGS += -Wbad-function-cast
CFLAGS += -Wcast-align
CFLAGS += -Wcast-qual
CFLAGS += -Wdeclaration-after-statement
CFLAGS += -Wformat=2
CFLAGS += -Wlogical-op
CFLAGS += -Wmissing-declarations
CFLAGS += -Wmissing-include-dirs
CFLAGS += -Wmissing-prototypes
CFLAGS += -Wnested-externs
CFLAGS += -Wpointer-arith
CFLAGS += -Wredundant-decls
CFLAGS += -Wsequence-point
CFLAGS += -Wshadow
CFLAGS += -Wstrict-prototypes
CFLAGS += -Wswitch
CFLAGS += -Wundef
CFLAGS += -Wunreachable-code
CFLAGS += -Wunused-but-set-parameter
CFLAGS += -Wwrite-strings
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
	$(CC) -pthread $(CFLAGS) rpthread.c -DTIMESLICE=$(TSLICE)
else ifeq ($(SCHED), MLFQ)
	$(CC) -pthread $(CFLAGS) rpthread.c -DMLFQ -DTIMESLICE=$(TSLICE)
else
	echo "no such scheduling algorithm"
endif

clean:
	rm -rf testfile *.o *.a
