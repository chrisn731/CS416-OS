/* syscall.c
 *
 * Group Members Names and NetIDs:
 *   1. Michael Nelli (mrn73)
 *   2. Christopher Naporlee (cmn134)
 *
 * ILab Machine Tested on: snow.cs.rutgers.edu
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/syscall.h>

double avg_time = 0;

int main(int argc, char *argv[]) {
	struct timeval start, end;
	int i;

	for (i = 0; i < 3000; i++) {
		gettimeofday(&start, NULL);
		syscall(SYS_getpid);
		gettimeofday(&end, NULL);
		avg_time += (double)(end.tv_usec - start.tv_usec) + (double)(end.tv_sec - start.tv_sec) * 1e6;
	}

	avg_time /= 3000;

	printf("Average time per system call is %f microseconds\n", avg_time);

	return 0;
}
