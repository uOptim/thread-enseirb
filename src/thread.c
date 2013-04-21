#define _GNU_SOURCE
#include <sched.h>
#include <ucontext.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>

#include <pthread.h>
#include <semaphore.h>
#include <valgrind/valgrind.h>

#include "queue.h"
#include "thread.h"

#ifndef NBKTHREADS
#define NBKTHREADS 1
#endif

#define CONTEXT_STACK_SIZE 64*1024 /* 64 KB stack size for contexts */
#define KTHREAD_STACK_SIZE 4*1024  /* 4 KB stack size for kernel threads */


struct thread {
	pid_t tid;
	ucontext_t uc;
	ucontext_t *uc_prev;

	STAILQ_ENTRY(thread) threads;

	int valgrind_stackid;
};


sem_t nbready;
pthread_mutex_t readymtx   = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t runningmtx = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t donemtx    = PTHREAD_MUTEX_INITIALIZER;
STAILQ_HEAD(threadqueue, thread) ready, running, done;


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

	t->tid = 0; // will be replaced by the kernel thread's tid when scheduled

	return t;
}


int _clone_func()
{
	pid_t tid;
	ucontext_t uc;
	struct thread *t;

	tid = syscall(SYS_gettid);

	while (1) {
		sem_wait(&nbready);
		fprintf(stderr, "running job\n");

		// pick a new job
		pthread_mutex_lock(&readymtx);
		t = STAILQ_FIRST(&ready);
		STAILQ_REMOVE_HEAD(&ready, threads);
		pthread_mutex_unlock(&readymtx);

		t->tid = tid;
		t->uc_prev = &uc;
		t->uc.uc_link = &uc;

		pthread_mutex_lock(&runningmtx);
		STAILQ_INSERT_TAIL(&running, t, threads);
		pthread_mutex_unlock(&runningmtx);

		// run
		swapcontext(&uc, &t->uc);
	}

	return 0;
}


static void _run(struct thread *th, void *(*func)(void*), void *funcarg)
{
	void *retval;
	retval = func(funcarg);
	//thread_exit(retval);
}


/******************************************/
/*       CONSTRUCTOR & DESTRUCTOR         */
/******************************************/
__attribute__((constructor))
static void __init()
{
	int i;
	pid_t tid;
	void *stack;

	fprintf(stderr, "INIT\n");

	// nb of threads in the ready queue
	sem_init(&nbready, 1, 0);

	STAILQ_INIT(&running);
	STAILQ_INIT(&ready);
	STAILQ_INIT(&done);

	// spawn kernel threads
	for (i = 0; i < NBKTHREADS; i++) {

		if (NULL == (stack = malloc(KTHREAD_STACK_SIZE))) {
			perror("malloc");
			break;
		}

		tid = clone(
			_clone_func, stack + KTHREAD_STACK_SIZE, 
			CLONE_SIGHAND | CLONE_VM | CLONE_THREAD | CLONE_PTRACE |
			CLONE_FILES | CLONE_FS | CLONE_IO | CLONE_SYSVSEM,
			NULL
		);
		
		if (tid == -1) {
			perror("clone");
			free(stack);
		} else {
			// help valgrind
			VALGRIND_STACK_REGISTER(stack, stack + KTHREAD_STACK_SIZE);
		}
	}
}


__attribute__((destructor))
static void __destroy()
{
}


/******************************************/
/*       IMPLEMENTATION FUNCTIONS         */
/******************************************/
int thread_create(thread_t *newthread, void *(*func)(void *), void *funcarg)
{
	if (NULL == (*newthread = _thread_new())){
		return -1;
	}

	getcontext(&(*newthread)->uc);
	(*newthread)->uc.uc_link = NULL;
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
		&(*newthread)->uc, (void (*)(void))_run, 3, *newthread, func, funcarg
	);

	pthread_mutex_lock(&readymtx);
	STAILQ_INSERT_TAIL(&ready, *newthread, threads);
	pthread_mutex_unlock(&readymtx);
	sem_post(&nbready);

	return 0;
}

int thread_yield(void)
{
	return 0;
}

int thread_join(thread_t thread, void **retval)
{
	return 0;
}

thread_t thread_self(void)
{
	pid_t tid;
	thread_t t = NULL;

	tid = syscall(SYS_gettid);

	pthread_mutex_lock(&runningmtx);
	STAILQ_FOREACH(t, &running, threads) {
		if (t->tid == tid) {
			break;
		}
	}
	pthread_mutex_unlock(&runningmtx);

	if (t == NULL) {
		fprintf(stderr, "ERROR: orphan thread\n");
	}

	return t;
}

void thread_exit(void *retval)
{
	return;
}

