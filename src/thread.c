#define _GNU_SOURCE
#include <sched.h>
#include <ucontext.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/syscall.h>

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <valgrind/valgrind.h>

#include <assert.h>

#include "queue.h"
#include "thread.h"

#ifndef NBKTHREADS
#define NBKTHREADS 2 // INCLUDING the main thread!
#endif

#define CONTEXT_STACK_SIZE 64*1024 /* 64 KB stack size for contexts */
#define KTHREAD_STACK_SIZE 4*1024  /* 4 KB stack size for kernel threads */

#define GETTID syscall(SYS_gettid)

static int maintid;

// hold pointers to stacks to free them in the destructor
static void *kthread_stacks[NBKTHREADS-1];


struct thread {
	// uc = own context
	// uc_prev = where to go on thread_exit()
	ucontext_t uc, uc_prev;

	char isdone;
	void *retval;

	struct thread *caller;
	TAILQ_ENTRY(thread) threads;

	pthread_mutex_t mtx;
	int valgrind_stackid;
};


struct kthread {
	pid_t id;
	struct thread *job;
};

struct kthread runningjobs[NBKTHREADS];

sem_t nbready;
unsigned int thcount = 1;
pthread_cond_t thcountcond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t thcountmtx = PTHREAD_MUTEX_INITIALIZER;

TAILQ_HEAD(threadqueue, thread) ready;
pthread_mutex_t readymtx = PTHREAD_MUTEX_INITIALIZER;


/******************************************/
/*       SOME UTILITY FUNCTIONS           */
/******************************************/
struct thread *_thread_new(void)
{
	struct thread *t;

	if (NULL == (t = malloc(sizeof *t))) {
		perror("malloc");
		return NULL;
	}

	//fprintf(stderr, "new thread: %p\n", t);

	t->isdone = 0;
	t->caller = NULL;
	t->retval = NULL;
	t->uc.uc_link = (void *) 0xdeadbeef;

	pthread_mutex_init(&t->mtx, NULL);
	pthread_mutex_lock(&t->mtx);

	return t;
}


static void _add_job(struct thread *t)
{
	pthread_mutex_lock(&readymtx);
	TAILQ_INSERT_TAIL(&ready, t, threads);
	pthread_mutex_unlock(&readymtx);

	pthread_mutex_unlock(&t->mtx);
	sem_post(&nbready);
}


static struct thread *_get_job(void)
{
	struct thread *t;

	pthread_mutex_lock(&readymtx);
	if (NULL != (t = TAILQ_FIRST(&ready))) {
		assert(!t->isdone);
		pthread_mutex_lock(&t->mtx);
		TAILQ_REMOVE(&ready, t, threads);
	}
	pthread_mutex_unlock(&readymtx);


	return t;
}


// Threads MUST call this function instead of swapcontext
static int _magicswap(struct thread *th)
{
	int rv;
	pid_t tid = GETTID;
	struct thread *self, *caller;

	{ /* in the calling thread */
		self = thread_self();

		assert(!th->isdone);

		// init next job
		th->caller = self;
		th->uc_prev = self->uc_prev;

		//fprintf(stderr, "* Magicswap in (tid %ld) %p --> %p\n",
				//GETTID, self, th);

		// let the successor know who he is
		int i;
		for (i = 0; i < NBKTHREADS; i++) {
			if (runningjobs[i].id == tid) {
				runningjobs[i].job = th;
			}
		}
	}

	// POOF 
	rv = swapcontext(&self->uc, &th->uc);

	if (rv) {
		perror("swapcontext");
	}

	{ /* in some thread, we don't know who we are yet */
		// release the thread that called swap
		self = thread_self();
		caller = self->caller;

		assert(!self->isdone);

		//fprintf(stderr, "* Magicswap out (tid %ld) now in %p\n",
				//GETTID, self);

		//fprintf(stderr, "* releasing caller from Magicswap %p\n", caller);
		if (!caller->isdone) {
			// add job will unlock the caller
			_add_job(caller);
		} else {
			pthread_mutex_unlock(&caller->mtx);
		}
	}

	return rv;
}


static int _clone_func()
{
	struct thread *t;
	pid_t tid = GETTID;

	// main loop
	while (1) {
		t = thread_self();
		if (t != NULL) {
			//fprintf(stderr, "* unlock from _clone_func\n");
			if (!t->isdone) {
				_add_job(t);
			}
		}

		// get a new job
		sem_wait(&nbready);
		t = _get_job();
		assert(t != NULL);

		int i;
		for (i = 0; i < NBKTHREADS; i++) {
			if (runningjobs[i].id == tid) {
				runningjobs[i].job = t;
			}
		}

		// swap
		t->caller = NULL;
		//fprintf(stderr, "* clone swapping\n");
		swapcontext(&t->uc_prev, &t->uc);
	}

	//fprintf(stderr, "clone dead\n");

	return 0;
}


static void _run(void *(*func)(void*), void *funcarg)
{
	struct thread *self, *caller;

	// release the thread that called swap
	self = thread_self();
	caller = self->caller;

	fprintf(stderr, "Job %p started\n", self);

	if (caller) {
		//fprintf(stderr, "* releasing caller from _run %p\n", caller);
		if (!caller->isdone) {
			// add job will unlock the caller
			_add_job(caller);
		} else {
			pthread_mutex_unlock(&caller->mtx);
		}
	}

	thread_exit(func(funcarg));
}


/******************************************/
/*       CONSTRUCTOR & DESTRUCTOR         */
/******************************************/
__attribute__((constructor(101)))
static void __init()
{
	int i;
	pid_t tid;
	void *stack;

	TAILQ_INIT(&ready);

	// remember which thread started everything
	maintid = GETTID;

	// add this thread to the list
	struct thread *th;
	if (NULL == (th = _thread_new())) {
		exit(EXIT_FAILURE);
	}
	th->uc_prev = th->uc;

	runningjobs[0].job = th;
	runningjobs[0].id = maintid;

	// spawn more kernel threads
	for (i = 0; i < NBKTHREADS-1; i++) {
		kthread_stacks[i] = NULL;

		if (NULL == (stack = malloc(KTHREAD_STACK_SIZE))) {
			perror("malloc");
			exit(EXIT_FAILURE);
		}

		tid = clone(
			_clone_func, stack + KTHREAD_STACK_SIZE, 
			CLONE_VM | CLONE_FILES | CLONE_FS | CLONE_SIGHAND | CLONE_IO |
			CLONE_SYSVSEM | CLONE_THREAD | SIGCHLD,
			NULL
		);
		
		if (tid == -1) {
			perror("clone");
			free(stack);
		} else {
			// help valgrind
			VALGRIND_STACK_REGISTER(stack, stack + KTHREAD_STACK_SIZE);
			kthread_stacks[i] = stack;
			runningjobs[i+1].id = tid;
			runningjobs[i+1].job = NULL;
		}
	}


}


__attribute__((destructor))
static void __destroy()
{
	//fprintf(stderr, "DESTROY\n");
	//fprintf(stderr, "All clones dead, cleaning up stacks etc...\n");
}


/******************************************/
/*       IMPLEMENTATION FUNCTIONS         */
/******************************************/
int thread_create(thread_t *newthread, void *(*func)(void *), void *funcarg)
{
	void *stack;

	if (NULL == (*newthread = _thread_new())){
		return -1;
	}

	if (NULL == (stack = malloc(CONTEXT_STACK_SIZE))) {
		perror("malloc");
		free(*newthread);
		return -1;
	}

	getcontext(&(*newthread)->uc);
	(*newthread)->uc.uc_stack.ss_sp = stack;
	(*newthread)->uc.uc_stack.ss_size = CONTEXT_STACK_SIZE;

	(*newthread)->valgrind_stackid =
		VALGRIND_STACK_REGISTER(
			(*newthread)->uc.uc_stack.ss_sp,
			(*newthread)->uc.uc_stack.ss_sp
			+ (*newthread)->uc.uc_stack.ss_size
		);
	
	makecontext(
		&(*newthread)->uc, (void (*)(void))_run, 2, func, funcarg
	);

	pthread_mutex_lock(&thcountmtx);
	thcount++;
	pthread_mutex_unlock(&thcountmtx);

	_add_job(*newthread);

	return 0;
}


int thread_yield(void)
{
	struct thread *next;

	thread_t self = thread_self();
	assert(self != NULL);

	if (!sem_trywait(&nbready)) {
		next = _get_job();
		assert(next != NULL);
		_magicswap(next);
	} else {
		//fprintf(stderr, "* yield: no other thread ready\n");
	}

	return 0;
}


int thread_join(thread_t thread, void **retval)
{
	int rv = 0;

	pthread_mutex_lock(&thread->mtx);
	while (!thread->isdone) {
		pthread_mutex_unlock(&thread->mtx);
		thread_yield();
		pthread_mutex_lock(&thread->mtx);
	}

	*retval = thread->retval;

	if (GETTID != maintid) {
		// libérer ressource
		VALGRIND_STACK_DEREGISTER(thread->valgrind_stackid);
		munmap(thread->uc.uc_stack.ss_sp, CONTEXT_STACK_SIZE);
	}

	free(thread);

	return rv;
}


thread_t thread_self(void)
{
	int i;
	pid_t tid = GETTID;
	for (i = 0; i < NBKTHREADS; i++) {
		if (runningjobs[i].id == tid) {
			return runningjobs[i].job;
		}
	}

	assert(0);
}


void thread_exit(void *retval)
{
	int cond;
	struct thread *next;

	thread_t self = thread_self();
	assert(self != NULL);

	self->isdone = 1;
	self->retval = retval;

	pthread_mutex_lock(&thcountmtx);
	thcount--;
	cond = (thcount == 0);
	pthread_mutex_unlock(&thcountmtx);

	if (cond) {
		pthread_mutex_unlock(&self->mtx);
		free(self);
		//fprintf(stderr, "***** EXIT *****\n");
		exit(EXIT_SUCCESS);
	} else {
		if (!sem_trywait(&nbready)) {
			next = _get_job();
			assert(next != NULL);
			_magicswap(next);
		}
		
		else {
			if (GETTID == maintid) {
				//fprintf(stderr, "MAIN fall back to the infinite loop\n");
				_clone_func();
			} else {
				//fprintf(stderr, "CLONE fall back to the infinite loop\n");
				swapcontext(&self->uc, &self->uc_prev);
			}
		}
	}

	// we should never reach this point
	assert(0);

	// only the main should reach this point
	exit(EXIT_SUCCESS);
}

