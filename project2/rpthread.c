// File:	rpthread.c

// List all group member's name:
// username of iLab:
// iLab Server:

#include "rpthread.h"

static struct scheduler *scheduler;
static volatile uint open_tid, open_mutid;

static void init_scheduler(void);
static void disable_clock(void);
static void enable_clock(void);
static void enqueue_job(tcb *);

static void startup_thread(void *(*func)(void *), void *arg)
{
	void *rval;

	rval = func(arg);
	rpthread_exit(rval);
}

/* create a new thread */
int rpthread_create(rpthread_t *thread, pthread_attr_t *attr,
		void *(*function)(void*), void *arg)
{
	int errval = 0;
	tcb *new_tcb;
	void *new_stack;

	/* We do not want to be disturbed while we are going through the motions */
	disable_clock();
	if (!thread || !function) {
		errval = -1;
		goto out;
	}
	if (attr)
		warnx("Passing attribute to %s, not implemented ignoring.",
					__FUNCTION__);
	if (!scheduler)
		init_scheduler();

	new_tcb = malloc(sizeof(*new_tcb));
	new_stack = malloc(SIGSTKSZ);
	if (!new_tcb || !new_stack)
		err(-1, "Error allocating %zu bytes.",
			!new_stack ? SIGSTKSZ : sizeof(*new_tcb));

	errval = getcontext(&new_tcb->context);
	if (errval) {
		warn("Error getting context for new thread");
		goto out;
	}

	/* Link new context to running context */
	new_tcb->context.uc_link = &scheduler->running->context;
	new_tcb->context.uc_stack.ss_flags = 0;
	new_tcb->context.uc_stack.ss_size = SIGSTKSZ;
	new_tcb->context.uc_stack.ss_sp = new_stack;
	new_tcb->status = READY;
	/* Nasty cast is to shut the compiler up */
	makecontext(&new_tcb->context, (void (*)()) startup_thread, 2,
						function, arg);
	*thread = open_tid++;
	enqueue_job(new_tcb);
out:
	enable_clock();
	return errval;
}

/* give CPU possession to other user-level threads voluntarily */
int rpthread_yield(void)
{
	// change thread state from Running to Ready
	// save context of this thread to its thread control block
	// wwitch from thread context to scheduler context

	return swapcontext(&scheduler->running->context, &scheduler->context);
}

/* terminate a thread */
void rpthread_exit(void *value_ptr)
{
	tcb *call_thread = scheduler->running;
	// Deallocated any dynamic memory created when starting this thread

	errx(0, "Made it into %s", __FUNCTION__);
	call_thread->rval = value_ptr;

}


/* Wait for thread termination */
int rpthread_join(rpthread_t thread, void **value_ptr)
{
	// wait for a specific thread to terminate
	// de-allocate any dynamic memory created by the joining thread

	// YOUR CODE HERE
	return 0;
}

#define LOCKED 1
#define UNLOCKED 0
/* initialize the mutex lock */
int rpthread_mutex_init(rpthread_mutex_t *mutex,
			const pthread_mutexattr_t *mutexattr)
{
	errx(0, "Made it into %s", __FUNCTION__);
	if (mutexattr)
		fprintf(stderr, "Mutex init given attribute argument. "
				"Not implemented, ignoring...");
	mutex->id = 1;
	mutex->status = UNLOCKED;
	return 0;
}

/* aquire the mutex lock */
int rpthread_mutex_lock(rpthread_mutex_t *mutex)
{
	// use the built-in test-and-set atomic function to test the mutex
	// if the mutex is acquired successfully, enter the critical section
	// if acquiring mutex fails, push current thread into block list and //
	// context switch to the scheduler thread

	// YOUR CODE HERE
	errx(0, "Made it into %s", __FUNCTION__);
	return 0;
}

/* release the mutex lock */
int rpthread_mutex_unlock(rpthread_mutex_t *mutex)
{
	// Release mutex and make it available again.
	// Put threads in block list to run queue
	// so that they could compete for mutex later.

	// YOUR CODE HERE
	errx(0, "Made it into %s", __FUNCTION__);
	return 0;
}


/* destroy the mutex */
int rpthread_mutex_destroy(rpthread_mutex_t *mutex)
{
	// Deallocate dynamic memory created in rpthread_mutex_init

	errx(0, "Made it into %s", __FUNCTION__);
	return 0;
}

/* Start clock for our preemption interrupts */
static void enable_clock(void)
{
	struct itimerval timer = {
		.it_interval = {0, TIMESLICE * 1000},
		.it_value = {0, TIMESLICE * 1000}
	};
	setitimer(ITIMER_PROF, &timer, NULL);
}

/* Disable clock for our preemption interrupts */
static void disable_clock(void)
{
	struct itimerval timer = {
		.it_interval = {0, 0},
		.it_value = {0, 0}
	};
	setitimer(ITIMER_PROF, &timer, NULL);
}

/*
 * Really basic ll queue for now just to get basic thread stuff running.
 * All jobs that are added are put on the tail end.
 */
static void enqueue_job(tcb *thread)
{
	struct tcb_list **tail = &scheduler->q_tail;
	struct tcb_list *new_node;

	new_node = malloc(sizeof(*new_node));
	if (!new_node)
		err(-1, "Error allocating %zu bytes.", sizeof(*new_node));
	new_node->thread = thread;
	new_node->next = NULL;

	/* Update the end of the list and the tail to point to the new end */
	if (*tail)
		(*tail)->next = new_node;
	*tail = new_node;

	/*
	 * If there is currently no head to our list,
	 * that means we prob just initialized.
	 * For now set the head to be the tail, so dequeue_job() works
	 */
	if (!scheduler->q_head)
		scheduler->q_head = scheduler->q_tail;
}

/*
 * Takes job off of the head of the ll queue.
 * For now, I have it to be if the head == NULL that is an error,
 * but that should probably be changed for different logic...
 */
static tcb *dequeue_job(void)
{
	struct tcb_list **parser = &scheduler->q_head;
	struct tcb_list *new_head;
	tcb *job;

	/* There should be at no point where our queue is empty. */
	if (!*parser)
		exit(-1);
	new_head = (*parser)->next;
	job = (*parser)->thread;
	free(*parser);
	*parser = new_head;
	return job;
}

/* Round Robin (RR) scheduling algorithm */
static void sched_rr(void)
{
	tcb *next_t, *running = scheduler->running;

	while ((next_t = dequeue_job()) != NULL && next_t->status != READY)
		enqueue_job(next_t);
	if (!next_t)
		return;

	next_t->status = SCHEDULED;
	running->status = READY;
	scheduler->running = next_t;
	enqueue_job(running);
	enable_clock();
	if (swapcontext(&scheduler->context, &next_t->context) < 0)
		write(STDOUT_FILENO, "Error swapping context!\n",
				sizeof("Error swapping context!\n") - 1);
}

/* Preemptive MLFQ scheduling algorithm */
static void sched_mlfq(void)
{
	// Your own implementation of MLFQ
	// (feel free to modify arguments and return types)
	err(0, "Made it into %s", __FUNCTION__);

}

/* scheduler */
static void schedule(void)
{
	// Every time when timer interrup happens, your thread library
	// should be contexted switched from thread context to this
	// schedule function

	// Invoke different actual scheduling algorithms
	// according to policy (RR or MLFQ)

	// if (sched == RR)
	//		sched_rr();
	// else if (sched == MLFQ)
	// 		sched_mlfq();

	/* Idek right now, too tired, just infinite loop */
	for (;;) {
#ifndef MLFQ
		sched_rr();
#else
		sched_mlfq();
#endif
	}

}

/*
 * Signal handler/preemption function.
 * Only job is to swap context to the scheduler.
 */
static void sched_preempt(int signum)
{
	disable_clock();
	write(1, "PREEMPT\n", 8);
	if (swapcontext(&scheduler->running->context, &scheduler->context) < 0) {
		write(STDOUT_FILENO, "Preemption swap to scheduler failed.\n", 37);
		exit(-1);
	}
}

/*
 * Called on our first pthread_create().
 * Creates our scheduler context, our "init" thread context, and sets up
 * signal handler for preemption.
 */
static void init_scheduler(void)
{
	tcb *init_thread;
	void *sched_stack;
	struct sigaction sa;

	scheduler = malloc(sizeof(*scheduler));
	init_thread = malloc(sizeof(*init_thread));
	sched_stack = malloc(SIGSTKSZ);
	if (!init_thread || !scheduler || !sched_stack)
		err(-1, "Error allocating memory during scheduler init");

	if (getcontext(&init_thread->context) < 0 ||
	    getcontext(&scheduler->context) < 0)
		err(-1, "Error getting context while initialzing scheduler");

	/* Set up our scheduler context */
	scheduler->context.uc_link = NULL;
	scheduler->context.uc_stack.ss_flags = 0;
	scheduler->context.uc_stack.ss_size = SIGSTKSZ;
	scheduler->context.uc_stack.ss_sp = sched_stack;
	makecontext(&scheduler->context, schedule, 0);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sched_preempt;
	if (sigaction(SIGPROF, &sa, NULL) < 0)
		err(-1, "Error setting sigaction");

	/* This thread should have tid of 0. Our "main" thread */
	init_thread->id = open_tid++;
	init_thread->status = SCHEDULED;
	scheduler->running = init_thread;
	scheduler->q_head = NULL;
	scheduler->q_tail = NULL;
}

