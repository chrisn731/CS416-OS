CC = gcc
CFLAGS = -g -c -m32
AR = ar -rc
RANLIB = ranlib

all: my_vm.a

my_vm.a: my_vm.o
	$(AR) libmy_vm.a my_vm.o
	$(RANLIB) libmy_vm.a

my_vm.o: my_vm.h my_vm.c
	$(CC)	$(CFLAGS)  my_vm.c -lm

clean:
	rm -rf *.o *.a
