#define _GNU_SOURCE
#include <sched.h>
#include <pthread.h>
#include <semaphore.h>

#include <unistd.h>      // gettid()
#include <sys/syscall.h> // gettid()

#include "queue.h"
#include "thread.h"

#include <stdlib.h>
#include <ucontext.h>

#include <valgrind/valgrind.h>

#ifndef NBKTHREADS
#define NBKTHREADS 2
#endif

#define KTHREAD_STACK_SIZE 16*1024


static struct thread mainthread;
static void *stacks[NBKTHREADS];


struct thread {
	pid_t tid;
	char isdone;
	void *retval;

	ucontext_t uc, prev;
	LIST_ENTRY(thread) threads; // liste de threads

	int valgrind_stackid;
};

/* TESTING */
sem_t nbready;
pthread_mutex_t queuesmtx = PTHREAD_MUTEX_INITIALIZER;

/* Generation structure de liste doublement chainée des threads */
LIST_HEAD(tqueue, thread) ready, running, done;


static int kthread_func(void *stackp)
{
	struct thread *t = NULL;

	fprintf(stderr, "Kernel thread created: tid = %ld!\n",
			syscall(SYS_gettid));

	while (1) {
		sem_wait(&nbready);
		fprintf(stderr, "Got a job!\n");

		// prendre un job de la liste des threads ready
		pthread_mutex_lock(&queuesmtx);
		if (NULL == (t = LIST_FIRST(&ready))) {
			break;
			pthread_mutex_unlock(&queuesmtx);
		}
		LIST_REMOVE(t, threads);
		LIST_INSERT_HEAD(&running, t, threads);
		pthread_mutex_unlock(&queuesmtx);

		// exécuter
		t->tid = syscall(SYS_gettid);
		t->prev.uc_link = NULL;
		t->prev.uc_stack.ss_sp = stackp;
		t->prev.uc_stack.ss_size = KTHREAD_STACK_SIZE;
		swapcontext(&t->prev, &t->uc);

		// remettre dans une liste
		pthread_mutex_lock(&queuesmtx);
		LIST_REMOVE(t, threads);
		if (t->isdone) {
			LIST_INSERT_HEAD(&done, t, threads);
		} else {
			LIST_INSERT_HEAD(&ready, t, threads);
			sem_post(&nbready);
		}
		pthread_mutex_unlock(&queuesmtx);
	}

	return 0;
}

__attribute__((constructor))
void __init()
{
	mainthread.tid = syscall(SYS_gettid);
	mainthread.isdone = 0;
	mainthread.retval = NULL;
	getcontext(&mainthread.uc);

	LIST_INIT(&done);
	LIST_INIT(&ready);
	LIST_INIT(&running);

	/* spawn a few kernel threads */
	int i;
	pid_t tid;
	void *stack;
	sem_init(&nbready, 1, 0);
	for (i = 0; i < NBKTHREADS; i++) {

		if (NULL == (stack = malloc(KTHREAD_STACK_SIZE))) {
			perror("malloc");
			continue;
		}

		tid = clone(
			kthread_func, stack + KTHREAD_STACK_SIZE, 
			CLONE_SIGHAND | CLONE_VM | CLONE_THREAD | CLONE_PTRACE |
			CLONE_FILES | CLONE_FS | CLONE_IO | CLONE_SYSVSEM,
			stack
		);

		VALGRIND_STACK_REGISTER(stack, stack+KTHREAD_STACK_SIZE);

		if (tid == -1) {
			perror("clone");
			free(stack);
			continue;
		}

		stacks[i] = stack;
	}
}

__attribute__((destructor))
static void __destroy()
{
	int i;
	for (i = 0; i < NBKTHREADS; i++) {
		VALGRIND_STACK_DEREGISTER(stacks[i]);
		free(stacks[i]);
		stacks[i] = NULL;
	}
}


// sert à capturer la valeur de retour des threads ne faisant pas de
// thread_exit() mais un return
static void _run(struct thread *th, void *(*func)(void*), void *funcarg)
{
	void *retval;
	retval = func(funcarg);
	thread_exit(retval);
}


thread_t thread_self(void)
{
	struct thread *t;
	pid_t tid = syscall(SYS_gettid);

	if (tid == mainthread.tid) {
		return &mainthread;
	}

	LIST_FOREACH(t, &running, threads) {
		if (t->tid == tid) {
			fprintf(stderr, "thread is running\n");
			return t;
		}
	}

	fprintf(stderr, "Orphan thread tid = %d\n", tid);
	return NULL;
}


int thread_create(thread_t *newthread, void *(*func)(void *), void *funcarg)
{
	*newthread = malloc(sizeof (struct thread));

	if (*newthread == NULL) {
		perror("malloc");
		return -1;
	}

	getcontext(&(*newthread)->uc);
	(*newthread)->tid = 0;
	(*newthread)->isdone = 0;
	(*newthread)->retval = NULL;
	(*newthread)->uc.uc_link = NULL;
	(*newthread)->uc.uc_stack.ss_size = 64*1024;

	(*newthread)->valgrind_stackid =
		VALGRIND_STACK_REGISTER(
			(*newthread)->uc.uc_stack.ss_sp,
			(*newthread)->uc.uc_stack.ss_sp
			+ (*newthread)->uc.uc_stack.ss_size
		);

  	(*newthread)->uc.uc_stack.ss_sp = malloc(64*1024);

  	makecontext(
		&(*newthread)->uc, (void (*)(void))_run, 3, *newthread, func, funcarg
	);

	// ajoute un job
	pthread_mutex_lock(&queuesmtx);
	LIST_INSERT_HEAD(&ready, *newthread, threads);
	pthread_mutex_unlock(&queuesmtx);
	sem_post(&nbready);

	return 0;
}


int thread_yield(void)
{
	int rv = 0;
	struct thread *self = thread_self();
	
	// yield depuis le main
	if (self == &mainthread) { 
		;
	} 

	// yield depuis un thread != du main
	else { 
		rv = swapcontext(&self->uc, &self->prev);
	}

	return rv;
}


int thread_join(thread_t thread, void **retval)
{
	int rv = 0;

	while (!thread->isdone) {
		rv = thread_yield();
	}

	*retval = thread->retval;

	if (thread != &mainthread) {
		// libérer ressource
		VALGRIND_STACK_DEREGISTER(thread->valgrind_stackid);
		free(thread->uc.uc_stack.ss_sp);
		free(thread);
	}

	return rv;
}


void thread_exit(void *retval)
{
	struct thread *self = thread_self();

	self->isdone = 1;
	self->retval = retval;

	if (self != &mainthread) {
		swapcontext(&self->uc, &self->prev);
	}

	else {
		do {
			thread_yield();
		} while (!LIST_EMPTY(&ready));
	}

	exit(0);
}

