#include "thread.h"
#include "queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>

#include <valgrind/valgrind.h>

#define TIMESLICE 200000

struct timeval start, end;
static char init = 0;
static struct sigaction alarm_scheduler;

static struct thread mainthread;
static struct thread *curthread  = &mainthread; // thread courrant
static struct thread *nextthread = NULL;        // thread suivant schedulé


struct thread {
	unsigned int id;
	char isdone;
	void *retval;

  int priority;
	ucontext_t uc;
	LIST_ENTRY(thread) threads; // liste de threads

	int valgrind_stackid;
};


/* Generation structure de liste doublement chainée des threads */
LIST_HEAD(tqueue, thread) ready, done;

			  static int _swap_thread(struct thread *th1, struct thread *th2);
			  static void _swap_scheduler(int sig);
			  static int _initialize_thread(thread_t *newthread, void* (*func)(void*), void *funcarg);



static void __init()
{
	mainthread.id = 0;
	mainthread.isdone = 0;
	mainthread.retval = NULL;

	getcontext(&mainthread.uc);

	LIST_INIT(&ready);
	init = 1;

	alarm_scheduler.sa_flags = SA_RESTART;
  	alarm_scheduler.sa_handler = _swap_scheduler;
	sigfillset(&alarm_scheduler.sa_mask);
	sigaddset(&alarm_scheduler.sa_mask, SIGALRM);

	if(sigaction(SIGALRM, &alarm_scheduler, NULL) == -1 ){
	  perror("[ERROR] sigaction initialization");
	  exit(2);
	}
	
	//ualarm(200000, 200000);
	printf("main : %p\n", &mainthread);
	ualarm(TIMESLICE, 0);
	gettimeofday(&start, NULL);

}




int _thread_yield(void)
{
	if (!init) __init();
	int rv = 0;
	struct thread *self = thread_self();

	//cas où on arrive en bout de liste : on reboucle sur la tete
	if (nextthread == NULL && !LIST_EMPTY(&ready)) {
	  nextthread = LIST_FIRST(&ready);
	}
	else
	  nextthread = LIST_NEXT(self, threads);
	// swapcontext si thread schedulé
	if (nextthread != NULL) {
	  fprintf(stderr, "prio : %d\n", self->priority);
	  rv = _swap_thread(self, nextthread);
	 
	
	  //  gettimeofday(&start, NULL);

	}

	return rv;
}

static void _swap_scheduler (int signal) {
  gettimeofday(&end, NULL);
  fprintf(stderr,"thread %p, prio %d : %ld\n", curthread, curthread->priority,((end.tv_sec * 1000000 + end.tv_usec)-(start.tv_sec * 1000000 + start.tv_usec)));	

  _thread_yield();
}


// Utiliser cette fonction pour éviter 1h de debugage à cause de curthread non
// mis à jour.
static int _swap_thread(struct thread *th1, struct thread *th2)
{
        

	int rv = 0;
	curthread = th2;
	ualarm(th2->priority * TIMESLICE, 0);
	gettimeofday(&start, NULL);
	rv = swapcontext(&th1->uc, &th2->uc);
	

	if (rv) {
		perror("swapcontext");
	}

	return rv;
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
	if (!init) __init();

	return curthread;
}




static int _initialize_thread_priority(thread_t *newthread, void *(*func)(void *), void *funcarg, int prio) {
	static unsigned int id = 1;

	if (!init) __init();

	*newthread = malloc(sizeof (struct thread));

	if (*newthread == NULL) {
		perror("malloc");
		return -1;
	}

	getcontext(&(*newthread)->uc);
	(*newthread)->id = id++;
	(*newthread)->isdone = 0;
	(*newthread)->retval = NULL;
	(*newthread)->priority = prio;
	(*newthread)->uc.uc_link = &mainthread.uc;
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

	return 0;
}

static int _initialize_thread(thread_t *newthread, void *(*func)(void *), void *funcarg) {
 return _initialize_thread_priority(newthread, func, funcarg, 1);
}


static void _insert_thread(thread_t newthread){
	LIST_INSERT_HEAD(&ready, newthread, threads);
}


int thread_create_priority(thread_t *newthread, void *(*func)(void *), void *funcarg, int prio)
{
  _initialize_thread_priority(newthread, func, funcarg, prio);
  _insert_thread(*newthread);
  return 0;
}

int thread_create(thread_t *newthread, void *(*func)(void *), void *funcarg){
  return thread_create_priority(newthread, func, funcarg, 1);
}

int thread_yield(void)
{

	if (!init) __init();

	int rv = 0;

	struct thread *self = thread_self();

	// yield depuis le main
	if (self == &mainthread) { 
	        //cas où on arrive en bout de liste : on reboucle sur la tete
		if (nextthread == NULL && !LIST_EMPTY(&ready)) {
		  nextthread = LIST_FIRST(&ready);
		}
		// swapcontext si thread schedulé
		if (nextthread != NULL) {
		  rv = _swap_thread(self, nextthread);
		}
		
		// sinon ne rien faire (rester dans main)
	} 

	// yield depuis un thread != du main
	else { 
		// màj thread suivant
		nextthread = LIST_NEXT(self, threads);
		// donner la main au mainthread
		rv = _swap_thread(self, &mainthread);
	}

	return rv;
}



int thread_join(thread_t thread, void **retval)
{
	if (!init) __init();

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
	if (!init) __init();

	struct thread *self = thread_self();

	self->isdone = 1;
	self->retval = retval;
	
	if (self != &mainthread) {
		// màj thread suivant
		nextthread = LIST_NEXT(self, threads);
			LIST_REMOVE(self, threads);

		// repasser au main
		_swap_thread(self, &mainthread);
	}

	else {
		do {		  
			thread_yield();
		} while (!LIST_EMPTY(&ready));
	}

	exit(0);
}

