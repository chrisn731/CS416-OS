/*
 * signal.c
 *
 * Group Members Names and NetIDs:
 *   1. Christopher Naporlee - cmn134
 *   2. Michael Nelli - mrn73
 *
 * ILab Machine Tested on: snow.cs.rutgers.edu
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

/* Part 1 - Step 2 to 4: Do your tricks here
 * Your goal must be to change the stack frame of caller (main function)
 * such that you get to the line after "r2 = *( (int *) 0 )"
 */
void segment_fault_handler(int signum)
{
	int *ptr = &signum;

	printf("I am slain!\n");
	/*
	 * CPU stores the signum at -0x14(%rbp).
	 * then from where the base pointer is to where the
	 * return address is located, there in lies 0xB8 bytes.
	 * So 0x14 + 0xB8 = 0xCC.
	 * 0xCC / sizeof(int) = 0x33.
	 * Add 0x33 to the ptr so it is now pointing to the return address
	 * of where we segfaulted.
	 * Now add 2 to that address so we skip over the bad instruction.
	 * 	- mov (%rax), %eax segfaults us and is 2 bytes.
	 */
	ptr += 0xCC / sizeof(*ptr);
	*ptr += 2;
}

int main(int argc, char *argv[])
{
	int r2 = 0;

	signal(SIGSEGV, segment_fault_handler);
	/* Part 1 - Step 1: Registering signal handler */
	/* Implement Code Here */

	r2 = *( (int *) 0 ); // This will generate segmentation fault

	printf("I live again!\n");

	return 0;
}
