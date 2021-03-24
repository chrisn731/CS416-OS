# Operating System Design
CS 416 - Operating System Design: The saga continues, this time Chris
and Mike learn about operating systems.

## Project 1 - C Programming Warmup
### Part 1: signal.c
Program that uses a signal handler to recover from a segmentation fault.
```
			+              +
			|      .       |
			|      .       |
			|      .       |
			+--------------+
			| Instruction  |
			|return address|
    &signum + 0xCC +--> +--------------+
			|      .       |
			|      .       |
			|      .       |
			|      .       |
			+--------------+
			|    signum    |
	   &signum +-->	+--------------+
```
Using the signal variable sent to our signal handler, we can traverse up the stack
and edit the return address to the instruction that we failed on.

### Part 2: syscall.c
Program that calls finds the average time of a system call. This average is
calculated by doing a system call > 1000 times. Result is in microseconds.

### Part 3: threads.c
Program that simply spins up two seperate threads, and uses mutexes to
increment a shared global variable to an inputted amount.<br/>
E.g. ./threads 10 will increment a global variable to 10 then exit.

## Project 2 - User Level Thread Library
A simple thread library implemented at the user level using two different types
of queues.

### Round Robin
Round robin holds a pointer to a current task and simply goes around a list
picking up the next task to run.
```
            task_1<----task_4
               |         ^
               |         |
               v         |
running---> task_2---->task_3
```

### Multi-Level Feedback Queue
MLFQ uses multiple queues within an array. The lower the value of the queue number
the higher the priority of the task, and they are run first. If there are multiple
tasks in a queue to be run, they run in round robin. Once they stop or become
blocked, the queue goes to lower levels and runs those tasks.
```
+------------------+
|                  |
|1->task_1         |
|                  |
|2->task_2->task_3 |
|                  |
|3->               |
|                  |
|4->task_4         |
|                  |
+------------------+
```

## Project 3 - User Level Memory Management
