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


// hold pointers to stacks to free them in the destructor
static void *kthread_stacks[NBKTHREADS-1];
static pid_t kthread_tids[NBKTHREADS-1];

struct thread {
	// uc = own context
	// uc_prev = where to go on thread_exit()
	ucontext_t uc, *uc_prev;

	char isdone;
	void *retval;

	struct thread *caller;
	TAILQ_ENTRY(thread) threads;

	pthread_mutex_t mtx;
	int valgrind_stackid;
};

pid_t maintid;
struct thread *mainth;

sem_t nbready;
unsigned int thcount = 1;
pthread_cond_t thcountcond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t thcountmtx = PTHREAD_MUTEX_INITIALIZER;

TAILQ_HEAD(threadqueue, thread) ready;
pthread_mutex_t readymtx = PTHREAD_MUTEX_INITIALIZER;

static void *curjobs[NBKTHREADS];
pthread_mutex_t jobsmtx = PTHREAD_MUTEX_INITIALIZER;


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

	fprintf(stderr, "new thread: %p\n", t);

	t->isdone = 0;
	t->caller = NULL;
	t->retval = NULL;
	t->uc_prev = (void *) 0xdeadbeef;
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

	if (t && pthread_mutex_lock(&t->mtx));

	return t;
}


static void _release_parrent(struct thread *t)
{
	struct thread *caller = t->caller;

	if (caller) {
		fprintf(stderr, "releasing %p\n", caller);
		if (!caller->isdone) {
			_add_job(caller);
		}
		pthread_mutex_unlock(&caller->mtx);
	}
}


// Threads MUST call this function instead of swapcontext
static int _magicswap(struct thread *oth, struct thread *th)
{
	int rv;
	fprintf(stderr, "Magicswap (tid %ld, maintid = %d)\n", GETTID, maintid);
	fprintf(stderr, "x-- oth = %p --> th = %p\n", oth, th);

	{ /* in the calling thread */
		assert(!th->isdone);
		// init next job
		th->uc_prev = oth->uc_prev;
		th->caller = oth;

		// let the successor know who he is
		pthread_mutex_lock(&jobsmtx);
		curjobs[GETTID % NBKTHREADS] = th;
		pthread_mutex_unlock(&jobsmtx);
	}

	// POOF 
	rv = swapcontext(&oth->uc, &th->uc);

	if (rv) {
		perror("swapcontext");
	}

	{ /* in some thread, we don't know who we are yet */
		// release the thread that called swap
		struct thread *self;
		self = thread_self();
		_release_parrent(self);
	}

	return rv;
}


static int _clone_func(void *arg)
{
	ucontext_t uc;
	struct thread *t;

	// main loop
	while (1) {
		// Release the job on this thread.
		// This is done first because this function may be called from the
		// main thread and we need to release the 'main' job.
		pthread_mutex_lock(&jobsmtx);
		t = curjobs[GETTID % NBKTHREADS];
		pthread_mutex_unlock(&jobsmtx);

		if (t) {
			fprintf(stderr, "unlock from _clone_func\n");
			if (!t->isdone) {
				_add_job(t);
			}
			pthread_mutex_unlock(&t->mtx);
		}

		// get a new job
		sem_wait(&nbready);
		t = _get_job();
		assert(t != NULL);

		// update job lists
		pthread_mutex_lock(&jobsmtx);
		curjobs[GETTID % NBKTHREADS] = t;
		pthread_mutex_unlock(&jobsmtx);

		// swap back here when nothing else to do (thread_exit())
		t->uc_prev = &uc;
		t->caller = NULL;

		// swap
		fprintf(stderr, "Clone swapping\n");
		swapcontext(&uc, &t->uc);
	}

	fprintf(stderr, "clone dead\n");

	return 0;
}


static void _run(void *(*func)(void*), void *funcarg)
{
	struct thread *self;

	// release the thread that called swap
	self = thread_self();
	_release_parrent(self);

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

	// init global variables
	for (i = 0; i < NBKTHREADS; i++) {
		curjobs[i] = NULL;
	}

	// remember which thread started everything
	maintid = GETTID;

	// spawn more kernel threads
	for (i = 0; i < NBKTHREADS-1; i++) {
		kthread_tids[i] = 0;
		kthread_stacks[i] = NULL;

		stack = mmap(
			NULL, KTHREAD_STACK_SIZE, PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_ANON, 0, 0
		);

		if (MAP_FAILED == stack) {
			perror("mmap");
			continue;
		}

		tid = clone(
			_clone_func, stack + KTHREAD_STACK_SIZE, 
			CLONE_VM | CLONE_FILES | CLONE_FS | CLONE_SIGHAND | 
			CLONE_SYSVSEM | CLONE_IO | CLONE_THREAD | SIGCHLD,
			NULL
		);
		
		if (tid == -1) {
			perror("clone");
			free(stack);
		} else {
			// help valgrind
			VALGRIND_STACK_REGISTER(stack, stack + KTHREAD_STACK_SIZE);
			kthread_stacks[i] = stack;
			kthread_tids[i] = tid;
		}
	}

	// add this thread to the list
	if (NULL == (mainth = _thread_new())) {
		exit(EXIT_FAILURE);
	}
	mainth->uc_prev = &mainth->uc;

	pthread_mutex_lock(&jobsmtx);
	curjobs[GETTID % NBKTHREADS] = mainth;
	pthread_mutex_unlock(&jobsmtx);
}


__attribute__((destructor))
static void __destroy()
{
	fprintf(stderr, "DESTROY\n");

	/*
	for (i = 0; i < NBKTHREADS-1; i++) {

		if (kthread_tids[i]) {
			fprintf(stderr, "Killing tid %d\n", kthread_tids[i]);
			if (kill(kthread_tids[i], SIGTERM)) {
				perror("kill");
			}

			fprintf(stderr, "Waiting for clone %d\n", kthread_tids[i]);
			pid = waitpid(kthread_tids[i], &status, 0);

			// check waitpid return value
			if (pid != kthread_tids[i]) {
				perror("waitpid");
			} else {
				fprintf(stderr, "Clone %d dead\n", kthread_tids[i]);
			}
		}
	}
	*/
	
	fprintf(stderr, "All clones dead, cleaning up stacks etc...\n");

	if (mainth != NULL) {
		pthread_mutex_unlock(&mainth->mtx);
		free(mainth);
	}
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

	stack = mmap(
		NULL, CONTEXT_STACK_SIZE, PROT_READ | PROT_WRITE,
		MAP_SHARED | MAP_ANON, 0, 0
	);

	if (MAP_FAILED == stack) {
		perror("mmap");
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
	pthread_mutex_unlock(&(*newthread)->mtx);

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

	pthread_mutex_unlock(&thread->mtx);
	free(thread);

	if (thread == mainth) {
		mainth = NULL;
	}

	return rv;
}


thread_t thread_self(void)
{
	thread_t self;

	pthread_mutex_lock(&jobsmtx);
	self = curjobs[GETTID % NBKTHREADS];
	pthread_mutex_unlock(&jobsmtx);

	return self;
}


void thread_exit(void *retval)
{
	fprintf(stderr, "thread exit\n");
	int i, cond;
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
		fprintf(stderr, "***** EXIT *****\n");
		exit(EXIT_SUCCESS);
	} else {
		if (!sem_trywait(&nbready)) {
			next = _get_job();
			assert(next != NULL);
			_magicswap(self, next);
		}
		
		else {
			if (GETTID == maintid) {
				fprintf(stderr, "MAIN fall back to the infinite loop\n");
				_clone_func(NULL);
			} else {
				fprintf(stderr, "CLONE fall back to the infinite loop\n");
				swapcontext(&self->uc, self->uc_prev);
			}
		}
	}

	// we should never reach this point
	assert(0);

	// only the main should reach this point
	exit(EXIT_SUCCESS);
}

