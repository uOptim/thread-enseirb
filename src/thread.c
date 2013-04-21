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
#define NBKTHREADS 1 // INCLUDING the main thread!
#endif

#define CONTEXT_STACK_SIZE 64*1024 /* 64 KB stack size for contexts */
#define KTHREAD_STACK_SIZE 4*1024  /* 4 KB stack size for kernel threads */


// hold pointers to stacks to free them in the destructor
static void *kthread_stacks[NBKTHREADS-1];

struct thread {
	pid_t tid;
	ucontext_t uc;

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

	t->tid = 0; // will be replaced by the kernel thread's tid when scheduled
	t->isdone = 0;
	t->retval = NULL;
	pthread_mutex_init(&t->mtx, NULL);

	return t;
}


static struct thread *_get_job(void)
{
	struct thread *t, *tmp;

	//fprintf(stderr, "================ get job ================\n");
	pthread_mutex_lock(&readymtx);
	t = TAILQ_FIRST(&ready);
	if (t != NULL) {
		//fprintf(stderr, " ---- tailq remove ---- \n");
		TAILQ_REMOVE(&ready, t, threads);
	}
	//TAILQ_FOREACH(tmp, &ready, threads) {
	//	fprintf(stderr, "\t thread in queue: %p - %d\n", tmp, tmp->tid);
	//}
	pthread_mutex_unlock(&readymtx);

	return t;
}


static void _add_job(struct thread *t)
{
	struct thread *tmp;
	pthread_mutex_lock(&readymtx);
	//fprintf(stderr, " ---- tailq add ---- \n");
	TAILQ_INSERT_TAIL(&ready, t, threads);
	//TAILQ_FOREACH(tmp, &ready, threads) {
	//	fprintf(stderr, "\t thread in queue: %p - %d\n", tmp, tmp->tid);
	//}
	pthread_mutex_unlock(&readymtx);
	sem_post(&nbready);
}


int _clone_func()
{
	pid_t tid;
	ucontext_t uc;
	struct thread *t;

	tid = syscall(SYS_gettid);

	while (1) {
		sem_wait(&nbready);

		t = _get_job();

		t->tid = tid;
		t->uc.uc_link = &uc;

		pthread_mutex_lock(&runningmtx);
		TAILQ_INSERT_TAIL(&running, t, threads);
		pthread_mutex_unlock(&runningmtx);

		swapcontext(&uc, &t->uc);
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
__attribute__((constructor))
static void __init()
{
	int i;
	pid_t tid;
	void *stack;

	// nb of threads in the ready queue
	sem_init(&nbready, 1, 0);

	TAILQ_INIT(&running);
	TAILQ_INIT(&ready);

	// spawn kernel threads
	for (i = 0; i < NBKTHREADS-1; i++) {

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
			kthread_stacks[i] = NULL;
		} else {
			// help valgrind
			VALGRIND_STACK_REGISTER(stack, stack + KTHREAD_STACK_SIZE);
			kthread_stacks[i] = stack;
		}
	}

	if (NULL == (mainthread = _thread_new())) {
		fprintf(stderr, "Could not initilialize thread library\n");
		exit(EXIT_FAILURE);
	}

	getcontext(&mainthread->uc);
	mainthread->tid = syscall(SYS_gettid);

	pthread_mutex_lock(&runningmtx);
	TAILQ_INSERT_TAIL(&running, mainthread, threads);
	pthread_mutex_unlock(&runningmtx);
}


__attribute__((destructor))
static void __destroy()
{
	int i;
	struct thread *t;

	while (!TAILQ_EMPTY(&ready)) {
	     t = TAILQ_FIRST(&ready);
	     TAILQ_REMOVE(&ready, t, threads);
		 if (t != mainthread) {
			 free(t->uc.uc_stack.ss_sp);
		 }
	     free(t);
     }

	while (!TAILQ_EMPTY(&running)) {
	     t = TAILQ_FIRST(&running);
	     TAILQ_REMOVE(&running, t, threads);
		 if (t != mainthread) {
			 free(t->uc.uc_stack.ss_sp);
		 }
	     free(t);
     }

	for (i = 0; i < NBKTHREADS-1; i++) {
		if (kthread_stacks[i] != NULL) {
			free(kthread_stacks[i]);
		}
	}
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
		&(*newthread)->uc, (void (*)(void))_run, 3, func, funcarg
	);

	_add_job(*newthread);
	return 0;
}


int thread_yield(void)
{
	struct thread *t;
	thread_t self = thread_self();

	t = _get_job();

	if (t != NULL) {

		pthread_mutex_lock(&runningmtx);
		TAILQ_REMOVE(&running, self, threads);
		TAILQ_INSERT_TAIL(&running, t, threads);
		pthread_mutex_unlock(&runningmtx);

		if (!self->isdone) {
			_add_job(self);
		}

		t->tid = self->tid;
		t->uc.uc_link = self->uc.uc_link;

		return swapcontext(&self->uc, &t->uc);
	}

	else if (self->isdone) {
		pthread_mutex_lock(&runningmtx);
		TAILQ_REMOVE(&running, self, threads);
		pthread_mutex_unlock(&runningmtx);

		fprintf(stderr, "pouet\n");
		//exit(EXIT_SUCCESS);
	}

	return 0;
}


int thread_join(thread_t thread, void **retval)
{
	int rv = 0;

	while (!thread->isdone) {
		rv = thread_yield();
	}

	*retval = thread->retval;
	pthread_mutex_unlock(&thread->mtx);

	if (thread != mainthread) {
		// libérer ressource
		VALGRIND_STACK_DEREGISTER(thread->valgrind_stackid);
		free(thread->uc.uc_stack.ss_sp);
		free(thread);
	}

	return rv;
}


thread_t thread_self(void)
{
	pid_t tid;
	thread_t t = NULL;

	tid = syscall(SYS_gettid);

	pthread_mutex_lock(&runningmtx);
	TAILQ_FOREACH(t, &running, threads) {
		if (t->tid == tid) {
			break;
		}
	}
	pthread_mutex_unlock(&runningmtx);

	if (t == NULL) {
		fprintf(stderr, "ERROR: orphan thread\n");
	//} else {
	//	fprintf(stderr, 
	//			"Thread %p\n\ttid = %d\n\tisdone = %d\n",
	//			t, t->tid, t->isdone);
	}

	return t;
}


void thread_exit(void *retval)
{
	thread_t self = thread_self();

	self->isdone = 1;
	self->retval = retval;
	thread_yield();

	exit(EXIT_SUCCESS);
}

