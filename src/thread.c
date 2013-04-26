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
#define NBKTHREADS 10 // INCLUDING the main thread!
#endif

#define CONTEXT_STACK_SIZE 64*1024 /* 64 KB stack size for contexts */
#define KTHREAD_STACK_SIZE 4*1024  /* 4 KB stack size for kernel threads */

#define GETTID getpid()


// hold pointers to stacks to free them in the destructor
static void *jobs[NBKTHREADS];
static char signals[NBKTHREADS];
static void *kthread_stacks[NBKTHREADS-1];
static pid_t kthread_tids[NBKTHREADS-1];

struct thread {
	ucontext_t uc, *uc_prev;

	char isdone;
	void *retval;

	TAILQ_ENTRY(thread) threads;

	pthread_mutex_t mtx;
	int valgrind_stackid;
};

static struct thread *mainthread;

sem_t nbready;
// new commandment: Thou shalt not copy mutexes on multiple stacks.
pthread_mutex_t jobsmtx = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t readymtx = PTHREAD_MUTEX_INITIALIZER;
TAILQ_HEAD(threadqueue, thread) ready;


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

	t->isdone = 0;
	t->retval = NULL;
	t->uc_prev = (void *) 0xdeadbeef;

	pthread_mutex_init(&t->mtx, NULL);


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
		TAILQ_REMOVE(&ready, t, threads);
	}
	pthread_mutex_unlock(&readymtx);

	return t;
}


static void _activate_job(struct thread *t)
{
	pthread_mutex_lock(&t->mtx);

	pthread_mutex_lock(&jobsmtx);
	jobs[GETTID % NBKTHREADS] = t;
	pthread_mutex_unlock(&jobsmtx);
}


static void _release_job(struct thread *t)
{
	if (!t->isdone) {
		_add_job(t);
	}

	pthread_mutex_unlock(&t->mtx);
}


void _kthread_sighandler(int sig)
{
	// if we are running a job, then release it before exiting
	thread_t self = thread_self();

	if (self != NULL) {
		if (pthread_mutex_trylock(&self->mtx) == EDEADLK) {
			_release_job(self);
		}
	}

	fprintf(stderr, "clone dead\n");
	exit(EXIT_SUCCESS);
}


static int _clone_func()
{
	ucontext_t uc;
	struct thread *t;
	
	// main loop
	while (1) {
		sem_wait(&nbready);

		// NULL jobs are signals to quit
		if (NULL == (t = _get_job())) {
			continue;
		}

		_activate_job(t);
		t->uc_prev = &uc;
		swapcontext(&uc, &t->uc);
		_release_job(t);
	}

	exit(EXIT_SUCCESS);
}


static void _run(void *(*func)(void*), void *funcarg)
{
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

	// semaphores and mutexes
	sem_init(&nbready, 1, 0);

	// init global variables
	for (i = 0; i < NBKTHREADS; i++) {
		jobs[i] = NULL;
		signals[i] = 0;
	}

	if (NULL == (mainthread = _thread_new())) {
		exit(EXIT_FAILURE);
	}

	// signal handler
	struct sigaction sa;
	sa.sa_flags = 0;
	sa.sa_handler = _kthread_sighandler;
	sigemptyset(&sa.sa_mask);
	sigaddset(&sa.sa_mask, SIGTERM);
	sigaction(SIGTERM, &sa, NULL);

	// spawn kernel threads
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
			CLONE_SYSVSEM | SIGCHLD,
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

	getcontext(&mainthread->uc);
	_activate_job(mainthread);
}


__attribute__((destructor))
static void __destroy()
{
	fprintf(stderr, "destroy\n");

	pid_t pid;
	int i, status;
	for (i = 0; i < NBKTHREADS-1; i++) {

		if (kthread_tids[i]) {

			if (kill(kthread_tids[i], SIGTERM)) {
				perror("kill");
			} else {
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
	}

	fprintf(stderr, "All clones dead, cleaning up stacks etc...\n");
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

	_add_job(*newthread);

	return 0;
}


int thread_yield(void)
{
	thread_t self = thread_self();

	assert(self != NULL);
	
	if (self == mainthread) {
		//_release_job(self);
		//_clone_func();
	} else {
		// return to the kernel thread's stack
		swapcontext(&self->uc, self->uc_prev);
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

	if (thread != mainthread) {
		// libérer ressource
		//VALGRIND_STACK_DEREGISTER(thread->valgrind_stackid);
		//munmap(thread->uc.uc_stack.ss_sp, CONTEXT_STACK_SIZE);
	}

	//pthread_mutex_unlock(&thread->mtx);
	//pthread_mutex_destroy(&thread->mtx);
	//free(thread);

	return rv;
}


thread_t thread_self(void)
{
	thread_t self;

	pthread_mutex_lock(&jobsmtx);
	self = jobs[GETTID % NBKTHREADS];
	pthread_mutex_unlock(&jobsmtx);

	return self;
}


void thread_exit(void *retval)
{
	thread_t self = thread_self();
	assert(self != NULL);

	self->isdone = 1;
	self->retval = retval;

	if (self == mainthread) {
		_release_job(self);
		_clone_func();
	} else {
		// return to the kernel thread's stack
		swapcontext(&self->uc, self->uc_prev);
		// we should never reach this point
		assert(0);
	}

	// only the main should reach this point
	exit(EXIT_SUCCESS);
}

