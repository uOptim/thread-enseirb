#include "thread.h"
#include "queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>

#include <assert.h>
#include <valgrind/valgrind.h>

#define TIMESLICE 100000

/* Affichage du temps pour la préemption avec priorité*/
struct timeval start, end;

static struct sigaction alarm_scheduler;

static struct thread *mainthread = NULL;
	static struct thread *curthread = NULL; // thread courrant


struct thread {
	char isdone;
	void *retval;

	int priority; 
	ucontext_t uc;
	LIST_ENTRY(thread) threads; // liste de threads

	int valgrind_stackid;
};


/* Generation structure de liste doublement chainée des threads */
LIST_HEAD(tqueue, thread) ready;

static int _swap_thread(struct thread *th1, struct thread *th2);
static void _swap_scheduler(int sig);


static struct thread *_thread_new()
{
	struct thread *t;

	if (NULL != (t = malloc(sizeof *t))) {
		t->isdone = 0;
		t->priority = 1;
		t->retval = NULL;
		getcontext(&t->uc);
	}

	return t;
}


__attribute__((constructor))
static void __init()
{
	mainthread = _thread_new();

	if (mainthread == NULL) {
		exit(EXIT_FAILURE);
	}

	curthread = mainthread;

	LIST_INIT(&ready);
	LIST_INSERT_HEAD(&ready, mainthread, threads);

//	alarm_scheduler.sa_flags = SA_RESTART;
//	alarm_scheduler.sa_handler = _swap_scheduler;
//	sigemptyset(&alarm_scheduler.sa_mask);
//	sigaddset(&alarm_scheduler.sa_mask, SIGALRM);

	if(sigaction(SIGALRM, &alarm_scheduler, NULL) == -1 ){
		perror("[ERROR] sigaction initialization");
		exit(2);
	}

	//ualarm(TIMESLICE, 0);
	/* Execution du main pendant TIMESLICE */
	fprintf(stderr, "Main -- priorité 1 par défaut \n");
	gettimeofday(&start, NULL);
}


__attribute__((destructor))
static void __destroy()
{
	free(mainthread);
}


static void _swap_scheduler (int signal) {
	thread_yield();
}


static int _swap_thread(struct thread *th1, struct thread *th2)
{
	int rv = 0;

	curthread = th2;
	assert(!th2->isdone);

	// afficher le temps d'exécution du threads précédent
//	gettimeofday(&end, NULL);
//	fprintf(stderr, "Execution reelle : %ld us\n", 
//			((end.tv_sec * 1000000 + end.tv_usec)
//			 -(start.tv_sec * 1000000 + start.tv_usec)));	
//
//	// demarrer le nouveau thread
//	gettimeofday(&start, NULL);
//	fprintf(stderr, "prio : %d (temps théorique d'execution : %d)\n",
//			th2->priority, (th2->priority * TIMESLICE));

	// Poof!
	rv = swapcontext(&th1->uc, &th2->uc);
	if (rv) {
		perror("swapcontext");
	}

	struct thread *self;
	self = thread_self();
	assert(self != NULL);
	assert(!self->isdone);

	// nouvelle alarme selon le timeslice
	//ualarm(curthread->priority * TIMESLICE, 0);

	return rv;
}


// sert à capturer la valeur de retour des threads ne faisant pas de
// thread_exit() mais un return
static void _run(void *(*func)(void*), void *funcarg)
{
	void *retval;

	// nouvelle alarme selon le timeslice
	//ualarm(curthread->priority * TIMESLICE, 0);
	
	assert(func);
	
	retval = func(funcarg);
	thread_exit(retval);
}


thread_t thread_self(void)
{
	return curthread;
}


static int _initialize_thread_priority(
	thread_t *newthread,
	void *(*func)(void *),
	void *funcarg,
	int prio
) {
	*newthread = _thread_new();

	if (*newthread == NULL) {
		perror("malloc");
		return -1;
	}

	(*newthread)->priority = (prio > 0) ? prio : 1;
	(*newthread)->uc.uc_link = NULL; //&mainthread.uc;
	(*newthread)->uc.uc_stack.ss_size = 64*1024;

	(*newthread)->valgrind_stackid =
		VALGRIND_STACK_REGISTER(
			(*newthread)->uc.uc_stack.ss_sp,
			(*newthread)->uc.uc_stack.ss_sp
			+ (*newthread)->uc.uc_stack.ss_size
			);

	(*newthread)->uc.uc_stack.ss_sp = malloc(64*1024);

	makecontext(
		&(*newthread)->uc, (void (*)(void))_run, 2, func, funcarg
	);

	return 0;
}


int thread_create_priority(thread_t *newthread, void *(*func)(void *), void *funcarg, int prio)
{
	int rv = 0;
	rv = _initialize_thread_priority(newthread, func, funcarg, prio);

	if (!rv) {
		LIST_INSERT_HEAD(&ready, *newthread, threads);
	}

	return rv;
}


int thread_create(thread_t *newthread, void *(*func)(void *), void *funcarg){
	return thread_create_priority(newthread, func, funcarg, 1);
}


int thread_yield(void)
{
	int rv = 0;

	struct thread *self;
	struct thread *next;

	self = thread_self();
	assert(self != NULL);

	assert(!LIST_EMPTY(&ready));
	next = LIST_NEXT(self, threads);

	//cas où on arrive en bout de liste : on reboucle sur la tete
	if (next == NULL) {
		next = LIST_FIRST(&ready);
	}

	assert(next);
	assert(!next->isdone);

	// swapcontext si nouveau thread schedulé
	if (next != curthread) {
		rv = _swap_thread(self, next);
	}

	// sinon ne rien faire

	return rv;
}



int thread_join(thread_t thread, void **retval)
{
	int rv = 0;

	while (!thread->isdone) {
		thread_yield();
	}

	*retval = thread->retval;

	if (thread != mainthread) {
		// libérer ressource
		VALGRIND_STACK_DEREGISTER(thread->valgrind_stackid);
		//free(thread->uc.uc_stack.ss_sp);
		//free(thread);
	}

	return rv;
}


void thread_exit(void *retval)
{
	struct thread *self = thread_self();
	assert(self != NULL);

	self->isdone = 1;
	self->retval = retval;

	LIST_REMOVE(self, threads);

	if (!LIST_EMPTY(&ready)) {
		thread_yield();
		assert(0);
	}

	exit(0);
}

