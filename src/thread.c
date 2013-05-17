#define _GNU_SOURCE
#include <ucontext.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/syscall.h>

#include <pthread.h>
#include <semaphore.h>
#include <valgrind/valgrind.h>

#include <assert.h>

#include "queue.h"
#include "thread.h"

#ifndef NBKTHREADS
#define NBKTHREADS 4 // INCLUDING the main thread!
#endif

#define CONTEXT_STACK_SIZE 32*1024 /* 32 KB stack size for contexts */
#define KTHREAD_STACK_SIZE 4*1024  /* 4 KB stack size for kernel threads */

#define GETTID syscall(SYS_gettid)

static int maintid;
ucontext_t mainfallback;

pthread_t kthreads[NBKTHREADS-1];


struct thread {
	ucontext_t uc, *uc_prev;

	char isdone;
	void *retval;

        int state;
        int canceled;

	struct thread *caller;  // points to the thread that called swapcontext

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


static pthread_key_t key_self;
static struct thread *_mainth;

static sem_t nbready;
static unsigned int thcount = 1; // one thread at start time
static pthread_mutex_t thcountmtx = PTHREAD_MUTEX_INITIALIZER;

static TAILQ_HEAD(threadqueue, thread) ready;
static pthread_mutex_t readymtx = PTHREAD_MUTEX_INITIALIZER;


/******************************************/
/*       SOME UTILITY FUNCTIONS           */
/******************************************/
struct thread *_thread_new(void)
{
	struct thread *t;

	t = malloc(sizeof *t);

	if (NULL == t) {
		perror("malloc");
		return NULL;
	}

	t->isdone = 0;
	t->state = THREAD_CANCEL_ENABLE;
	t->canceled = 0;
	t->caller = NULL;
	t->retval = NULL;
	t->uc_prev = NULL;
	t->uc.uc_link = NULL;

	pthread_mutex_init(&t->mtx, NULL);
	pthread_mutex_lock(&t->mtx);

	return t;
}


static void _add_job(struct thread *t)
{
	if (0 == t->canceled || THREAD_CANCEL_DISABLE == t->state)
	{
		pthread_mutex_lock(&readymtx);
		TAILQ_INSERT_TAIL(&ready, t, threads);
		pthread_mutex_unlock(&t->mtx);
		pthread_mutex_unlock(&readymtx);
		
		sem_post(&nbready);
		
	} else {

		t->isdone = 0;
		t->retval = NULL;

		pthread_mutex_lock(&thcountmtx);
		thcount--;
		pthread_mutex_unlock(&thcountmtx);

		if (t != _mainth) {
			// libérer ressource
			VALGRIND_STACK_DEREGISTER(t->valgrind_stackid);
			free(t->uc.uc_stack.ss_sp);
			pthread_mutex_unlock(&t->mtx);
			free(t);
		} else {
			// special case for the main t (see __destroy)
			pthread_mutex_unlock(&t->mtx);
			free(t);
			_mainth = NULL;
		}
	}
}


static struct thread *_get_job(void)
{
	struct thread *t;

	pthread_mutex_lock(&readymtx);
	if (NULL != (t = TAILQ_FIRST(&ready))) {
		assert(!t->isdone);
		TAILQ_REMOVE(&ready, t, threads);
		pthread_mutex_lock(&t->mtx);
	}
	pthread_mutex_unlock(&readymtx);

	return t;
}


// Threads MUST call this function instead of swapcontext
static int _magicswap(struct thread *self, struct thread *th)
{
	int rv = 0;

	{ /* in the calling thread */
		assert(th);
		assert(!th->isdone);

#ifdef SWAPINFO
		fprintf(stderr, "* Magicswap in (tid %ld) %p --> %p\n",
				GETTID, self, th);
#endif

		// init next job
		th->caller = self;
		th->uc_prev = self->uc_prev;

		pthread_setspecific(key_self, th);
	}

	// POOF 
	swapcontext(&self->uc, &th->uc);

	//if (rv) {
	//	perror("swapcontext");
	//}

	{ /* in some thread, we don't know who we are yet */
		struct thread *caller, *called;

		// release the thread that called swap
		called = thread_self();
		caller = called->caller;

		assert(!called->isdone);

#ifdef SWAPINFO
		fprintf(stderr, "* Magicswap out (tid %ld) now in %p\n",
				GETTID, called);
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


static void * _clone_func(void *arg)
{
	ucontext_t uc;
	struct thread *t;

	// main loop
	while (1) {
		// release the job that called us if any
		t = thread_self();
		if (t) {
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
		assert(!t->isdone);

		// swap
		t->uc_prev = &uc;
		t->caller = NULL;

		// update 'self' thread
		pthread_setspecific(key_self, t);

		swapcontext(&uc, &t->uc);
	}

	pthread_exit(NULL);
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
	int i, rv;

	TAILQ_INIT(&ready);
	sem_init(&nbready, 1, 0);

	// remember which thread started everything
	maintid = GETTID;

	// add this thread to the list
	if (NULL == (_mainth = _thread_new())) {
		exit(EXIT_FAILURE);
	}

	getcontext(&_mainth->uc);
	_mainth->uc_prev = &_mainth->uc;

	// init fallback for the main thread
	void *stack = malloc(CONTEXT_STACK_SIZE);
	if (!stack) {
		perror("malloc");
		exit(EXIT_FAILURE);
	}
	getcontext(&mainfallback);
	mainfallback.uc_stack.ss_sp = stack;
	mainfallback.uc_stack.ss_size = CONTEXT_STACK_SIZE;

	VALGRIND_STACK_REGISTER(
		mainfallback.uc_stack.ss_sp,
		mainfallback.uc_stack.ss_sp
		+ mainfallback.uc_stack.ss_size
	);
	makecontext(&mainfallback, (void (*)(void))_clone_func, 1, NULL);

	// per-thread data
	pthread_key_create(&key_self, NULL);
	pthread_setspecific(key_self, _mainth); // 'self' is now _mainth

	// spawn more kernel threads
	for (i = 0; i < NBKTHREADS-1; i++) {
		rv = pthread_create(&kthreads[i], NULL, _clone_func, NULL);

		if (rv != 0) {
			perror("pthread_create");
			exit(EXIT_FAILURE);
		}

		pthread_detach(kthreads[i]);
	}
}


__attribute__((destructor))
static void __destroy()
{
	free(mainfallback.uc_stack.ss_sp);

	// special case for the main thread that may not be joined or may not call
	// thread_exit()
	if (_mainth) {
		pthread_mutex_unlock(&_mainth->mtx);
		free(_mainth);
	}
}


/******************************************/
/*       IMPLEMENTATION FUNCTIONS         */
/******************************************/
thread_t thread_self(void)
{
	return pthread_getspecific(key_self);
}


int thread_create(thread_t *newthread, void *(*func)(void *), void *funcarg)
{
	void *stack;

	if (NULL == (*newthread = _thread_new())){
		return -1;
	}

	stack = malloc(CONTEXT_STACK_SIZE);
	if (NULL == stack) {
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
		_magicswap(self, next);
	} else {
#ifdef SWAPINFO
		fprintf(stderr, "* yield: no other thread ready\n");
#endif
	}

	return 0;
}

int thread_setcancelstate(int state, int *oldstate)
{
	struct thread *self = thread_self();
	
	assert(self != NULL);

	if (oldstate) {
		*oldstate = self->state;
	}
	
	self->state = state;
	
	return 0;
}

int thread_cancel(thread_t thread)
{
	assert(thread != NULL);
	
	if (thread_self() == thread) {
		thread->canceled = 1;	
	} else {
		pthread_mutex_lock(&thread->mtx);
		thread->canceled = 1;
		pthread_mutex_unlock(&thread->mtx);
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

	if (thread != _mainth) {
		// libérer ressource
		VALGRIND_STACK_DEREGISTER(thread->valgrind_stackid);
		free(thread->uc.uc_stack.ss_sp);
		pthread_mutex_unlock(&thread->mtx);
		free(thread);
	} else {
		// special case for the main thread (see __destroy)
		pthread_mutex_unlock(&thread->mtx);
		free(thread);
		_mainth = NULL;
	}
	
	return rv;
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

		if (self != _mainth) {
			VALGRIND_STACK_DEREGISTER(self->valgrind_stackid);
			//free(self->uc.uc_stack.ss_sp);
			free(self);
		} else {
			// special case for _mainth
			free(self);
			_mainth = NULL;
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
				swapcontext(&self->uc, &mainfallback);
			} else {
#ifdef SWAPINFO
				fprintf(stderr, "CLONE fall back to the infinite loop\n");
#endif
				swapcontext(&self->uc, self->uc_prev);
			}
		}
	}

	// we should never reach this point
	assert(0);
}

