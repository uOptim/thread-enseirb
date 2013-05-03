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

#define CONTEXT_STACK_SIZE 32*1024 /* 64 KB stack size for contexts */
#define KTHREAD_STACK_SIZE 4*1024  /* 4 KB stack size for kernel threads */

#define GETTID syscall(SYS_gettid)

static int maintid;

// hold pointers to stacks to free them in the destructor
static void *kthread_stacks[NBKTHREADS-1];


struct kthread {
	// this must be set ONLY ONCE and must be the return value of clone
	pid_t id; 
	// this points to the 'struct thread' job currently running
	struct thread *job;
	// context of the kthread
	ucontext_t uc;
} runningjobs[NBKTHREADS];


struct thread {
	// uc = own context
	ucontext_t uc;

	char isdone;
	void *retval;

	struct thread *caller;  // points to the thread that called swapcontext
	struct kthread *runner; // must point to an entry in 'runningjobs'

	TAILQ_ENTRY(thread) threads;

	pthread_mutex_t mtx;
	int valgrind_stackid;

	// NOTE:
	// * When run, a thread will attempt to unlock whatever is pointed by
	// caller. Make sure to set this to NULL if the swapcontext is done from
	// the stack of a kernel thread as opposed to the stack of a context. If
	// the swapcontext is done from another thread's context, make it point to
	// that thread.
};

struct thread *mainth;

sem_t nbready;
unsigned int thcount = 1; // one thread at start time
pthread_mutex_t thcountmtx = PTHREAD_MUTEX_INITIALIZER;

TAILQ_HEAD(threadqueue, thread) ready;
pthread_mutex_t readymtx = PTHREAD_MUTEX_INITIALIZER;

// NOTE: after hours of debugging, I found out that pthread does additionnal
// stuff to make malloc aware of concurrent memory allocation and that
// bypassing this by using clone() makes malloc unaware of multiple memory
// allocation request at the same time which causes deadlocks (from malloc's
// internals) among other problems such as heap corruption. A quick and dirty
// way is to force one malloc at a time with the following mutex.
pthread_mutex_t mallocmtx = PTHREAD_MUTEX_INITIALIZER;


/******************************************/
/*       SOME UTILITY FUNCTIONS           */
/******************************************/
struct thread *_thread_new(void)
{
	struct thread *t;

	pthread_mutex_lock(&mallocmtx);
	t = malloc(sizeof *t);
	pthread_mutex_unlock(&mallocmtx);

	if (NULL == t) {
		perror("malloc");
		return NULL;
	}

	t->isdone = 0;
	t->caller = NULL;
	t->retval = NULL;
	t->runner = NULL;
	t->uc.uc_link = NULL;

	pthread_mutex_init(&t->mtx, NULL);
	pthread_mutex_lock(&t->mtx);

	return t;
}


static void _add_job(struct thread *t)
{
	pthread_mutex_lock(&readymtx);
	TAILQ_INSERT_TAIL(&ready, t, threads);
	pthread_mutex_unlock(&t->mtx);
	pthread_mutex_unlock(&readymtx);

	sem_post(&nbready);
}


static struct thread *_get_job(void)
{
	struct thread *t;

	pthread_mutex_lock(&readymtx);
	if (NULL != (t = TAILQ_FIRST(&ready))) {
		assert(!t->isdone);
		TAILQ_REMOVE(&ready, t, threads);
	}
	pthread_mutex_unlock(&readymtx);

	if (t) {
		pthread_mutex_lock(&t->mtx);
	}

	return t;
}


// Threads MUST call this function instead of swapcontext
static int _magicswap(struct thread *self, struct thread *th)
{
	int rv;
	struct thread *caller;

	{ /* in the calling thread */
		assert(th);
		assert(!th->isdone);

#ifdef SWAPINFO
		fprintf(stderr, "* Magicswap in (tid %ld) %p --> %p\n",
				GETTID, self, th);
#endif

		// init next job
		th->caller = self;
		th->runner = self->runner;

		// update kthread entry
		self->runner->job = th;
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

#ifdef SWAPINFO
		fprintf(stderr, "* Magicswap out (tid %ld) now in %p\n",
				GETTID, self);
#endif

		// caller may be NULL in the following scenario:
		// th1 calls _magicswap and goes to sleep when calling swapcontext. It
		// is then unlocked by th2 which he called. th2 adds th1 to the job
		// queue. A thread that falled back to _clone_func dequeue th1 and
		// resumes it.
		if (caller) {
#ifdef SWAPINFO
			fprintf(stderr, "* releasing caller from Magicswap %p\n", caller);
#endif
			if (!caller->isdone) {
				// add job will unlock the caller
				_add_job(caller);
			} else {
				pthread_mutex_unlock(&caller->mtx);
			}
		}
	}

	return rv;
}


static int _clone_func(void *arg)
{
	struct thread *t;
	struct kthread *kself;

	// write our id to our assigned kthread entry
	kself = (struct kthread *) arg;
	kself->id = GETTID;

	// main loop
	while (1) {
		// release the job that called us if any
		t = kself->job;
		if (t != NULL) {
#ifdef SWAPINFO
			fprintf(stderr, "* unlock from _clone_func %p\n", t);
#endif
			if (!t->isdone) {
				_add_job(t);
			} else {
				pthread_mutex_unlock(&t->mtx);
			}
		}

		// get a new job
		sem_wait(&nbready);
		t = _get_job();
		assert(t != NULL);

		// update kthread entry
		kself->job = t;

		// swap
		t->caller = NULL;
		t->runner = kself;
		swapcontext(&kself->uc, &t->uc);
	}

	return 0;
}


static void _run(void *(*func)(void*), void *funcarg)
{
	struct thread *self, *caller;

	// release the thread that called swap
	self = thread_self();
	caller = self->caller;

#ifdef SWAPINFO
	fprintf(stderr, "Job %p started\n", self);
#endif

	if (caller) {
#ifdef SWAPINFO
		fprintf(stderr, "* releasing caller from _run %p\n", caller);
#endif
		if (!caller->isdone) {
			// add job will unlock the caller
			_add_job(caller);
		} else {
			pthread_mutex_unlock(&caller->mtx);
		}
	}

	void *retval;
	retval = func(funcarg);
	thread_exit(retval);
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
	sem_init(&nbready, 1, 0);

	// remember which thread started everything
	maintid = GETTID;

	// add this thread to the list
	if (NULL == (mainth = _thread_new())) {
		exit(EXIT_FAILURE);
	}
	mainth->runner = &runningjobs[0];

	// init kthread entries
	for (i = 0; i < NBKTHREADS; i++) {
		runningjobs[i].id = 0;
		runningjobs[i].job = NULL;
	}

	runningjobs[0].id = maintid;
	runningjobs[0].job = mainth;

	// spawn more kernel threads
	for (i = 0; i < NBKTHREADS-1; i++) {
		kthread_stacks[i] = NULL;

		pthread_mutex_lock(&mallocmtx);
		if (NULL == (stack = malloc(KTHREAD_STACK_SIZE))) {
			perror("malloc");
			exit(EXIT_FAILURE);
		}
		pthread_mutex_unlock(&mallocmtx);

		tid = clone(
			_clone_func, stack + KTHREAD_STACK_SIZE, 
			CLONE_VM | CLONE_FILES | CLONE_FS | CLONE_SIGHAND | CLONE_IO |
			CLONE_SYSVSEM | CLONE_THREAD,
			&runningjobs[i+1] // runningjobs[0] given to the main kthread
		);
		
		if (tid == -1) {
			perror("clone");
			pthread_mutex_lock(&mallocmtx);
			free(stack);
			pthread_mutex_unlock(&mallocmtx);
		} else {
			// help valgrind
			VALGRIND_STACK_REGISTER(stack, stack + KTHREAD_STACK_SIZE);
			// remember the pointer so we can free it later
			kthread_stacks[i] = stack;
		}
	}
}


__attribute__((destructor))
static void __destroy()
{
	int i;

	pthread_mutex_lock(&mallocmtx);
	for (i = 0; i < NBKTHREADS-1; i++) {
		free(kthread_stacks[i]);
	}

	// special case for the main thread that may not be joined or may not call
	// thread_exit()
	if (mainth) {
		pthread_mutex_unlock(&mainth->mtx);
		free(mainth);
	}
	pthread_mutex_unlock(&mallocmtx);
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

	pthread_mutex_lock(&mallocmtx);
	stack = malloc(CONTEXT_STACK_SIZE);
	pthread_mutex_unlock(&mallocmtx);

	if (NULL == stack) {
		perror("malloc");
		pthread_mutex_lock(&mallocmtx);
		free(*newthread);
		pthread_mutex_unlock(&mallocmtx);
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
		_magicswap(self, next);
	} else {
#ifdef SWAPINFO
		fprintf(stderr, "* yield: no other thread ready\n");
#endif
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

	pthread_mutex_lock(&mallocmtx);
	if (thread != mainth) {
		// libérer ressource
		VALGRIND_STACK_DEREGISTER(thread->valgrind_stackid);
		free(thread->uc.uc_stack.ss_sp);
		pthread_mutex_unlock(&thread->mtx);
		free(thread);
	} else {
		// special case for the main thread (see __destroy)
		pthread_mutex_unlock(&thread->mtx);
		free(thread);
		mainth = NULL;
	}
	pthread_mutex_unlock(&mallocmtx);
	
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
		// last thread just died, clean up
		pthread_mutex_unlock(&self->mtx);

		if (self != mainth) {
			VALGRIND_STACK_DEREGISTER(self->valgrind_stackid);
			//free(self->uc.uc_stack.ss_sp);
			free(self);
		} else {
			// special case for mainth
			free(self);
			mainth = NULL;
		}

		exit(EXIT_SUCCESS);
	}

	else {
		// this wasn't the last thread, either swap to another thread if
		// possible or fallback to the _clone_func to wait for new jobs.
		if (!sem_trywait(&nbready)) {
			next = _get_job();
			assert(next != NULL);
			_magicswap(self, next);
		}
		
		else {
			if (GETTID == maintid) {
#ifdef SWAPINFO
				fprintf(stderr, "MAIN fall back to the infinite loop\n");
#endif
				_clone_func(&runningjobs[0]);
			} else {
#ifdef SWAPINFO
				fprintf(stderr, "CLONE fall back to the infinite loop\n");
#endif
				swapcontext(&self->uc, &self->runner->uc);
			}
		}
	}

	// we should never reach this point
	assert(0);
}

