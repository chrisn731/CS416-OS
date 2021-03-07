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
static unsigned int time_elapsed;

static void init_scheduler(void);
static void disable_preempt(void);
static void enable_preempt(void);
static void enqueue_job(tcb *, struct tcb_list **);

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
	if (!thread || !function)
		return -1;
	if (attr)
		warnx("Passing thread attribute to %s, not implemented ignoring.",
						__func__);
	if (!scheduler) {
		init_scheduler();
		enable_preempt();
	}

	new_tcb = malloc(sizeof(*new_tcb));
	new_stack = malloc(SIGSTKSZ);
	if (!new_tcb || !new_stack)
		err(-1, "%s: Error allocating %zu bytes.",
			__func__, !new_stack ? SIGSTKSZ : sizeof(*new_tcb));

	errval = getcontext(&new_tcb->context);
	if (errval) {
		warnx("Error getting context for new thread");
		free(new_tcb);
		free(new_stack);
		return -1;
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
	enqueue_job(new_tcb, &scheduler->q[0]);
	return 0;
}

/* give CPU possession to other user-level threads voluntarily */
int rpthread_yield(void)
{
	/*
	 * change thread state from Running to Ready
	 * save context of this thread to its thread control block
	 * switch from thread context to scheduler context
	 */
	disable_preempt();
	return swapcontext(&scheduler->running->context, &scheduler->context);
}

/* Fetch a pointer to a thread control block given by a thread id. */
static tcb *fetch_tcb(rpthread_t twait_id)
{
	int i;

	for (i = 0; i < NUM_QS; i++) {
		struct tcb_list *parser, *head = scheduler->q[i];
		parser = head;
		while (head) {
		    	parser = parser->next;
			if (parser->thread->id == twait_id)
				return parser->thread;
		    	if (parser == head)
			    	break;
		}
	}
	return NULL;
}

/* Add a thread (waiter) to a thread's join list. */
static void add_to_thread_waitlist(struct tcb_list **join_list, tcb *waiter)
{
	struct tcb_list *new_waiter;

	new_waiter = malloc(sizeof(*new_waiter));
	if (!new_waiter)
		err(-1, "%s: Error allocating %zu bytes",
			__func__, sizeof(*new_waiter));
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

	disable_preempt();
	call_thread->rval = value_ptr;
	call_thread->status = STOPPED;
	release_wait_list(call_thread->join_list);
	call_thread->join_list = NULL;
	if (setcontext(&scheduler->context) < 0)
		err(-1, "%s: Error exiting thread.", __func__);
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
		disable_preempt();
		add_to_thread_waitlist(&join_thread->join_list, call_thread);
		call_thread->status = BLOCKED;
		swapcontext(&call_thread->context, &scheduler->context);
	}
	/* Grab the return value and free all memory */
	if (value_ptr)
		*value_ptr = join_thread->rval;
	join_thread->status = JOINED;
	return 0;
}

/* initialize the mutex lock */
int rpthread_mutex_init(rpthread_mutex_t *mutex, const pthread_mutexattr_t *mutexattr)
{
	if (!mutex)
		return -1;
	if (mutexattr)
		warnx("%s given attribute argument. "
			"Not implemented, ignoring...", __func__);
	mutex->id = open_mutid++;
	mutex->wait_list = NULL;
	mutex->owner = NULL;
	return 0;
}

/*
 * Puts a @new node in between @curr, and curr's next node (@next)
 * curr <-> next  to  curr <-> new <-> next
 */
static void list_add_post(struct tcb_list *new, struct tcb_list *curr,
							struct tcb_list *next)
{
	new->next = next;
	new->prev = curr;
	curr->next = new;
	next->prev = new;
}

/*
 * Puts a @new node in between @curr, and curr's previous node (@prev).
 * prev <-> curr  to  prev <-> new <-> curr
 */
static void list_add_prev(struct tcb_list *new, struct tcb_list *curr,
			  				struct tcb_list *prev)
{
	new->next = curr;
	new->prev = prev;
	curr->prev = new;
	prev->next = new;
}

/*
 * Links @prev's next ptr to @next and links @next's prev ptr to prev.
 * prev <-> to_del <-> next  to  prev <-> next
 */
static void list_del_curr(struct tcb_list *prev, struct tcb_list *next)
{
	prev->next = next;
	next->prev = prev;
}


/* aquire the mutex lock */
int rpthread_mutex_lock(rpthread_mutex_t *mutex)
{
	tcb *call_thread = scheduler->running;

	if (!mutex)
		return -1;
	/*
	 * If the mutex is taken, join the wait list and give control back
	 * to the scheduler. Also, when we swap back, we don't want the compiler
	 * to be smart and try and save one copy of the owner. We want to grab it
	 * every time.
	 */
	while ((volatile tcb *) mutex->owner) {
		struct tcb_list *waiting_thread;

		disable_preempt();
		call_thread->status = BLOCKED;
		waiting_thread = malloc(sizeof(*waiting_thread));
		if (!waiting_thread)
			err(-1, "%s: Error allocating %zu bytes.",
				__func__, sizeof(*waiting_thread));
		waiting_thread->thread = call_thread;
		waiting_thread->next = mutex->wait_list;
		mutex->wait_list = waiting_thread;
		swapcontext(&scheduler->running->context, &scheduler->context);
	}
	mutex->owner = call_thread;
	return 0;
}

/* release the mutex lock */
int rpthread_mutex_unlock(rpthread_mutex_t *mutex)
{
	sigset_t x;

	if (mutex->owner != scheduler->running)
		return -1;
	/*
	 * We do not want to be preeempted while we are setting other
	 * threads to the ready state. If we get preempted, during the release
	 * bad things might happen...
	 * Therefore, block the preemption signal and then check to see if
	 * the signal is sent after we are done.
	 */
	sigemptyset(&x);
	sigaddset(&x, SIGPROF);
	sigprocmask(SIG_BLOCK, &x, NULL);
	mutex->owner = NULL;
	release_wait_list(mutex->wait_list);
	mutex->wait_list = NULL;
	sigprocmask(SIG_UNBLOCK, &x, NULL);
	return 0;
}


/* destroy the mutex */
int rpthread_mutex_destroy(rpthread_mutex_t *mutex)
{
	if (mutex->owner)
		return -1;
	release_wait_list(mutex->wait_list);
	return 0;
}

/* Start clock for our preemption interrupts */
static void enable_preempt(void)
{
	struct itimerval timer = {
		.it_interval = {0, 0},
		.it_value = {0, TIMESLICE * 1000}
	};

	setitimer(ITIMER_PROF, &timer, NULL);
}

/* Disable clock for our preemption interrupts */
static void disable_preempt(void)
{
	struct itimerval timer = {
		.it_interval = {0, 0},
		.it_value = {0, 0}
	};

	setitimer(ITIMER_PROF, &timer, NULL);
}



/*
 * enqueue_job - puts @thread at the end of the queue.
 * @thread: pointer to a thread to be enqueued.
 * @q: pointer to the head of the list.
 */
static void enqueue_job(tcb *thread, struct tcb_list **q)
{
	struct tcb_list **tail = q;
	struct tcb_list *new_node;

	new_node = malloc(sizeof(*new_node));
	if (!new_node)
		err(-1, "%s: Error allocating %zu bytes.",
			__func__, sizeof(*new_node));
	new_node->thread = thread;
	new_node->next = new_node;
	new_node->prev = new_node;

 	/* Update the end of the list and the tail to point to the new end */
	if (!*tail)
		*tail = new_node;
	list_add_prev(new_node, *tail, (*tail)->prev);
}

/*
 * dequeue_job - returns a pointer to a tcb that has a status of READY
 * 			or NULL if no job found.
 * @q: Pointer to the head of the queue
 */
static tcb *dequeue_job(struct tcb_list **q)
{
	struct tcb_list **cursor = q;
	struct tcb_list *head = *cursor;
	tcb *job;

	/* There should be at no point where our queue is empty. */
	if (!*cursor)
		exit(-1);
	while ((*cursor)->thread->status != READY) {
		if ((*cursor)->thread->status == JOINED) {
			struct tcb_list *to_free = *cursor;
			list_del_curr(to_free->prev, to_free->next);
			*cursor = (*cursor)->next;
			free(to_free->thread->context.uc_stack.ss_sp);
			free(to_free->thread);
			free(to_free);
			if (to_free == *cursor) {
				/* The free'd node, is the only node in the list */
				*q = NULL;
				return NULL;
			}
			if (to_free == head) {
				/* The free'd node, is the head of the list */
				head = (*cursor);
				continue;
			}
		} else {
			*cursor = (*cursor)->next;
		}

		if (head == *cursor) {
			return NULL;
		}
	}
	job = (*cursor)->thread;
	*cursor = (*cursor)->next;
	return job;
}

/* Round Robin (RR) scheduling algorithm */
static int sched_rr(struct tcb_list **q)
{
	tcb *next_thread, *running = scheduler->running;

	/* If the our running became blocked, don't switch it's state */
	if (running->status == SCHEDULED)
		running->status = READY;

	//make sure next thread returned isnt null
	next_thread = dequeue_job(q);
	if (!next_thread)
		return -1;
	next_thread->status = SCHEDULED;
	scheduler->running = next_thread;
	time_elapsed = 0;
	enable_preempt();
	if (swapcontext(&scheduler->context, &next_thread->context) < 0)
		write(STDOUT_FILENO, "Error swapping context!\n",
				sizeof("Error swapping context!\n") - 1);
	return 0;
}

/* Preemptive MLFQ scheduling algorithm */
static void sched_mlfq(void)
{
	unsigned int i = scheduler->priority;
	struct tcb_list *moving_tcb_list, *new_tcb_list;

	/* Check if we gotta demote our thread */
	if ((i < NUM_QS - 1) && time_elapsed) {
		moving_tcb_list = scheduler->q[i]->prev;
		if (moving_tcb_list->next == moving_tcb_list)
			scheduler->q[i] = NULL;
		else
			list_del_curr(moving_tcb_list->prev, moving_tcb_list->next);

		new_tcb_list = scheduler->q[i+1];
		if (new_tcb_list == NULL) {
			scheduler->q[i+1] = moving_tcb_list;
			moving_tcb_list->next = moving_tcb_list;
			moving_tcb_list->prev = moving_tcb_list;
		} else {
			list_add_prev(moving_tcb_list, scheduler->q[i+1], scheduler->q[i+1]->prev);
		}
	}

	for (i = 0; i < NUM_QS; i++) {
		if (scheduler->q[i]) {
			scheduler->priority = i;
			if (sched_rr(&scheduler->q[i]) != -1)
				return;
		}
	}
	/* Should never be reached */
	errx(-1, "%s: All threads were BLOCKED or STOPPED", __func__);
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

	for (;;) {
#ifndef MLFQ
		sched_rr(scheduler->q);
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
	disable_preempt();
	/* If our alarm was called, we used our entire time slice */
	time_elapsed = 1;
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
	int i;

	scheduler = malloc(sizeof(*scheduler));
	init_thread = malloc(sizeof(*init_thread));
	sched_stack = malloc(SIGSTKSZ);
	if (!init_thread || !scheduler || !sched_stack)
		err(-1, "%s: Error allocating memory", __func__);

	if (getcontext(&init_thread->context) < 0 ||
	    getcontext(&scheduler->context) < 0)
		err(-1, "%s: Error getting context", __func__);

	/* Set up our scheduler context */
	scheduler->context.uc_link = NULL;
	scheduler->context.uc_stack.ss_flags = 0;
	scheduler->context.uc_stack.ss_size = SIGSTKSZ;
	scheduler->context.uc_stack.ss_sp = sched_stack;
	makecontext(&scheduler->context, schedule, 0);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sched_preempt;
	if (sigaction(SIGPROF, &sa, NULL) < 0)
		err(-1, "%s: Error setting sigaction", __func__);

	/* This thread should have tid of 0. Our "main" thread */
	init_thread->id = open_tid++;
	init_thread->status = SCHEDULED;
	init_thread->join_list = NULL;
	scheduler->running = init_thread;
	scheduler->priority = 0;
	for (i = 0; i < NUM_QS; i++)
		scheduler->q[i] = NULL;
	enqueue_job(init_thread, &scheduler->q[0]);
}
