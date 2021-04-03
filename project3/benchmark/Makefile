
all : test
test: ../my_vm.h
	gcc test.c -L../ -lmy_vm -m32 -o test

multi:
	gcc multi_test.c -L../ -lmy_vm -pthread -m32 -o test

clean:
	rm -rf test
