#define _GNU_SOURCE
#include <sched.h>
#include <ucontext.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
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
#define NBKTHREADS 3 // INCLUDING the main thread!
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

	char ismain;
	char isdone;
	void *retval;

	TAILQ_ENTRY(thread) threads;

	pthread_mutex_t mtx;
	int valgrind_stackid;
};

static struct thread *mainthread;

sem_t nbready;
pthread_mutex_t readymtx   = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t runningmtx = PTHREAD_MUTEX_INITIALIZER;
TAILQ_HEAD(threadqueue, thread) ready, running;


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
	t->ismain = 0;
	t->retval = NULL;
	getcontext(&t->uc);

	if (pthread_mutex_init(&t->mtx, NULL)) {
		perror("pthread_mutex_init");
	}

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
	struct thread *t/*, *tmp*/;

	pthread_mutex_lock(&readymtx);
	if (NULL != (t = TAILQ_FIRST(&ready))) {
		TAILQ_REMOVE(&ready, t, threads);
	}
	pthread_mutex_unlock(&readymtx);

	return t;
}


static void _release_job(struct thread *t)
{
	pthread_mutex_lock(&runningmtx);
	TAILQ_REMOVE(&running, t, threads);
	pthread_mutex_unlock(&runningmtx);

	if (!t->isdone) {
		_add_job(t);
	}

	pthread_mutex_unlock(&t->mtx);
}


static void _activate_job(struct thread *t)
{
	pthread_mutex_lock(&t->mtx);

	jobs[GETTID % NBKTHREADS] = t;

	pthread_mutex_lock(&runningmtx);
	TAILQ_INSERT_TAIL(&running, t, threads);
	pthread_mutex_unlock(&runningmtx);
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
		if (sem_wait(&nbready) && errno == EINTR) {
			break;
		}

		// NULL jobs are signals to quit
		if (NULL == (t = _get_job())) {
			break;
		}

		_activate_job(t);
		t->uc_prev = &uc;
		swapcontext(&uc, &t->uc);
		_release_job(t);
	}

	return 0;
}


static void _run(void *(*func)(void*), void *funcarg)
{
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

	TAILQ_INIT(&running);
	TAILQ_INIT(&ready);

	// nb of threads in the ready queue
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

		if (NULL == (stack = malloc(KTHREAD_STACK_SIZE))) {
			perror("malloc");
			continue;
		}

		tid = clone(
			_clone_func, stack + KTHREAD_STACK_SIZE, 
			CLONE_VM | CLONE_PTRACE | CLONE_FILES | CLONE_FS |
			CLONE_SIGHAND | CLONE_IO | SIGCHLD,
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

	mainthread->ismain = 1;
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
	if (NULL == (*newthread = _thread_new())){
		return -1;
	}

	pthread_mutex_lock(&(*newthread)->mtx);

	(*newthread)->uc.uc_link = NULL;
	(*newthread)->uc_prev = &(*newthread)->uc;
	(*newthread)->uc.uc_stack.ss_size = CONTEXT_STACK_SIZE;
	(*newthread)->uc.uc_stack.ss_sp = malloc(CONTEXT_STACK_SIZE);

	if (NULL == (*newthread)->uc.uc_stack.ss_sp) {
		perror("malloc");
		free(*newthread);
		return -1;
	}

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
	pthread_mutex_unlock(&(*newthread)->mtx);

	return 0;
}


int thread_yield(void)
{
	int rv = 0;
	struct thread *t;
	thread_t self = thread_self();

	assert(self != NULL);

	if (0 == sem_trywait(&nbready)) {

		t = _get_job();
		assert(t != NULL);

		_activate_job(t);
		t->uc_prev = self->uc_prev;
		_release_job(self);

		// switch to another thread
		rv = swapcontext(&self->uc, &t->uc);
	}

	/* Conflict with thread_exit()
	if (self->isdone) {
		_release_job(self);
		// return to the kernel thread's stack
		swapcontext(&self->uc, self->uc_prev);
		// we should NEVER reach this point
		assert(0);
	}
	*/

	// no job ready, not done, so continue
	return rv;
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

	if (!thread->ismain) {
		// libérer ressource
		VALGRIND_STACK_DEREGISTER(thread->valgrind_stackid);
		//free(thread->uc.uc_stack.ss_sp);
	}

	//pthread_mutex_unlock(&thread->mtx);
	//pthread_mutex_destroy(&thread->mtx);
	//free(thread);

	return rv;
}


thread_t thread_self(void)
{
	return jobs[GETTID % NBKTHREADS];
}


void thread_exit(void *retval)
{
	thread_t self = thread_self();
	assert(self != NULL);

	self->isdone = 1;
	self->retval = retval;

	if (self->ismain) {
		_release_job(self);
		_clone_func();
	} else {
		_release_job(self);
		// return to the kernel thread's stack
		swapcontext(&self->uc, self->uc_prev);
		// we should NEVER reach this point
		assert(0);
	}

	exit(EXIT_SUCCESS);
}

