/*
 * File: rpthread.h
 *
 * List all group member's name:
 * 	Christopher Naporlee - cmn134
 * 	Michael Nelli - mrn73
 *
 * iLab Server: snow.cs.rutgers.edu
 */
#ifndef RTHREAD_T_H
#define RTHREAD_T_H

#define _GNU_SOURCE

/* To use Linux pthread Library in Benchmark, you have to comment the USE_RTHREAD macro */
#define USE_RTHREAD 1

#ifndef TIMESLICE
/*
 * defined timeslice to 5 ms, feel free to change this while testing your code
 * it can be done directly in the Makefile
 */
#define TIMESLICE 5
#endif

#ifdef MLFQ
#define NUM_QS 4
#else
#define NUM_QS 1
#endif

#define READY 0
#define SCHEDULED 1
#define BLOCKED 2
#define STOPPED 4
#define JOINED	8

/* include lib header files that you need here: */
#include <err.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

typedef uint rpthread_t;

struct tcb_list;

typedef struct threadControlBlock {
	/* Probably need to add a priority field in here for MLFQ */
	struct tcb_list *join_list;
	void *rval;
	ucontext_t context;
	rpthread_t id;
	unsigned int status;
} tcb;

struct tcb_list {
	tcb *thread;
	struct tcb_list *next;
	struct tcb_list *prev;
};

/* mutex struct definition */
typedef struct rpthread_mutex_t {
	tcb *owner;
	struct tcb_list *wait_list;
	unsigned int id;
} rpthread_mutex_t;


struct scheduler {
	tcb *running;
	struct tcb_list *q[NUM_QS];
	//struct tcb_list *mlfq[NUM_QS];
	ucontext_t context;
	unsigned int priority;
};


/* Function Declarations: */

/* create a new thread */
int rpthread_create(rpthread_t * thread, pthread_attr_t * attr,
		void *(*function)(void*), void * arg);

/* give CPU pocession to other user level threads voluntarily */
int rpthread_yield(void);

/* terminate a thread */
void rpthread_exit(void *value_ptr);

/* wait for thread termination */
int rpthread_join(rpthread_t thread, void **value_ptr);

/* initial the mutex lock */
int rpthread_mutex_init(rpthread_mutex_t *mutex, const pthread_mutexattr_t *mutexattr);

/* aquire the mutex lock */
int rpthread_mutex_lock(rpthread_mutex_t *mutex);

/* release the mutex lock */
int rpthread_mutex_unlock(rpthread_mutex_t *mutex);

/* destroy the mutex */
int rpthread_mutex_destroy(rpthread_mutex_t *mutex);

#ifdef USE_RTHREAD
#define pthread_t rpthread_t
#define pthread_mutex_t rpthread_mutex_t
#define pthread_create rpthread_create
#define pthread_exit rpthread_exit
#define pthread_join rpthread_join
#define pthread_mutex_init rpthread_mutex_init
#define pthread_mutex_lock rpthread_mutex_lock
#define pthread_mutex_unlock rpthread_mutex_unlock
#define pthread_mutex_destroy rpthread_mutex_destroy
#endif

#endif /* RTHREAD_T_H */
