/*
 * File: rpthread.c
 *
 * List all group member's name:
 * 	Christopher Naporlee - cmn134
 * 	Michael Nelli - mrn73
 *
 * iLab Server: snow.cs.rutgers.edu
 */

#include "rpthread.h"

static struct scheduler *scheduler;
static uint open_tid, open_mutid;

static void init_scheduler(void);
static void disable_clock(void);
static void enable_clock(void);
static void enqueue_job(tcb *);

static void startup_thread(void *(*func)(void *), void *arg)
{
	rpthread_exit(func(arg));
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
		if (!scheduler)
			return errval;
		goto out;
	}
	if (attr)
		warnx("Passing thread attribute to %s, not implemented ignoring.",
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
		warnx("Error getting context for new thread");
		free(new_tcb);
		free(new_stack);
		goto out;
	}

	/* Link new context to running context */
	new_tcb->context.uc_link = &scheduler->running->context;
	new_tcb->context.uc_stack.ss_flags = 0;
	new_tcb->context.uc_stack.ss_size = SIGSTKSZ;
	new_tcb->context.uc_stack.ss_sp = new_stack;
	/* Nasty cast is to shut the compiler up */
	makecontext(&new_tcb->context, (void (*)(void)) startup_thread, 2,
						function, arg);
	new_tcb->status = READY;
	new_tcb->join_list = NULL;
	new_tcb->id = open_tid;
	*thread = open_tid++;
	enqueue_job(new_tcb);
out:
	enable_clock();
	return errval;
}

/* give CPU possession to other user-level threads voluntarily */
int rpthread_yield(void)
{
	/*
	 * change thread state from Running to Ready
	 * save context of this thread to its thread control block
	 * switch from thread context to scheduler context
	 */
	disable_clock();
	return swapcontext(&scheduler->running->context, &scheduler->context);
}

/* Fetch a pointer to a thread control block given by a thread id. */
static tcb *fetch_tcb(rpthread_t twait_id)
{
	struct tcb_list *parser = scheduler->q_head;

	while (parser && parser->thread->id != twait_id)
		parser = parser->next;
	return parser ? parser->thread : NULL;
}

/* Add a thread (waiter) to a thread's join list. */
static void add_to_thread_waitlist(struct tcb_list **join_list, tcb *waiter)
{
	struct tcb_list *new_waiter;

	new_waiter = malloc(sizeof(*new_waiter));
	if (!new_waiter)
		err(-1, "Error allocating %zu bytes", sizeof(*new_waiter));
	new_waiter->thread = waiter;
	new_waiter->next = *join_list;
	*join_list = new_waiter;
}

/*
 * Release a thread's join list, setting all the threads that are waiting
 * to ready.
 */
static void release_wait_list(struct tcb_list *wait_list)
{
	struct tcb_list *temp;

	for (; wait_list; wait_list = temp) {
		temp = wait_list->next;
		wait_list->thread->status = READY;
		free(wait_list);
	}
}

/* terminate a thread */
void rpthread_exit(void *value_ptr)
{
	tcb *call_thread = scheduler->running;

	disable_clock();
	call_thread->rval = value_ptr;
	call_thread->status = STOPPED;
	release_wait_list(call_thread->join_list);
	call_thread->join_list = NULL;
	if (setcontext(&scheduler->context) < 0)
		err(-1, "Error exiting thread.");
}


/* Wait for thread termination */
int rpthread_join(rpthread_t thread, void **value_ptr)
{
	tcb *join_thread, *call_thread = scheduler->running;

	if (thread >= open_tid)
		return -1;

	join_thread = fetch_tcb(thread);
	if (!join_thread)
		return -1;
	/*
	 * It's possible the thread we are waiting for has stopped already.
	 * If it hasn't stopped, block and wait on it. Otherwise, return.
	 */
	if (join_thread->status != STOPPED) {
		disable_clock();
		add_to_thread_waitlist(&join_thread->join_list, call_thread);
		call_thread->status = BLOCKED;
		swapcontext(&call_thread->context, &scheduler->context);
	}
	/* Grab the return value and free all memory */
	if (value_ptr)
		*value_ptr = join_thread->rval;
	free(join_thread->context.uc_stack.ss_sp);
	free(join_thread);
	return 0;
}

#define LOCKED 1
#define UNLOCKED 0
/* initialize the mutex lock */
int rpthread_mutex_init(rpthread_mutex_t *mutex, const pthread_mutexattr_t *mutexattr)
{
	if (!mutex)
		return -1;
	if (mutexattr)
		warnx("%s given attribute argument. "
			"Not implemented, ignoring...", __FUNCTION__);
	mutex->id = open_mutid++;
	mutex->owner = NULL;
	return 0;
}


/* aquire the mutex lock */
int rpthread_mutex_lock(rpthread_mutex_t *mutex)
{
	tcb *call_thread = scheduler->running;

	if (!mutex)
		return -1;
	/*
	 * If the thread is taken, join the wait list and give control back
	 * to the scheduler.
	 */
	if (mutex->owner) {
		struct tcb_list *waiting_thread;

		disable_clock();
		call_thread->status = BLOCKED;
		waiting_thread = malloc(sizeof(*waiting_thread));
		if (!waiting_thread)
			err(-1, "Error allocating %zu bytes.", sizeof(*waiting_thread));
		waiting_thread->thread = call_thread;
		waiting_thread->next = mutex->wait_list;
		mutex->wait_list = waiting_thread;
		rpthread_yield();
	}
	mutex->owner = call_thread;
	return 0;
}

/* release the mutex lock */
int rpthread_mutex_unlock(rpthread_mutex_t *mutex)
{
	/* Make sure whoever is calling this function is the owner */
	if (mutex->owner != scheduler->running)
		return -1;
	mutex->owner = NULL;
	release_wait_list(mutex->wait_list);
	mutex->wait_list = NULL;
	return 0;
}


/* destroy the mutex */
int rpthread_mutex_destroy(rpthread_mutex_t *mutex)
{
	if (mutex->owner)
		return -1;
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
	struct tcb_list *new_head, **head = &scheduler->q_head;
	tcb *job;

	/* There should be at no point where our queue is empty. */
	if (!*head)
		exit(-1);
	new_head = (*head)->next;
	job = (*head)->thread;
	free(*head);
	*head = new_head;
	return job;
}

/* Round Robin (RR) scheduling algorithm */
static void sched_rr(void)
{
	tcb *next_thread, *running = scheduler->running;

	/*
	 * We should probably put finished threads in their own list to
	 * stop our scheduler from considering them, but for now just keep them.
	 */
	enqueue_job(running);
	/* If the our running became blocked, don't switch it's state */
	if (running->status == SCHEDULED)
		running->status = READY;

	while ((next_thread = dequeue_job()) != NULL && next_thread->status != READY) {
		enqueue_job(next_thread);
	}
	if (!next_thread)
		return;

	next_thread->status = SCHEDULED;
	scheduler->running = next_thread;
	enable_clock();
	if (swapcontext(&scheduler->context, &next_thread->context) < 0)
		write(STDOUT_FILENO, "Error swapping context!\n",
				sizeof("Error swapping context!\n") - 1);
}

/* Preemptive MLFQ scheduling algorithm */
static void sched_mlfq(void)
{
	// Your own implementation of MLFQ
	// (feel free to modify arguments and return types)
	errx(0, "Made it into %s", __FUNCTION__);

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
	init_thread->join_list = NULL;
	scheduler->running = init_thread;
	scheduler->q_head = NULL;
	scheduler->q_tail = NULL;
}
