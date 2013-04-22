#include "thread.h"
#include "queue.h"

#include <stdlib.h>
#include <ucontext.h>

#include <valgrind/valgrind.h>


static struct thread mainthread;
static struct thread *curthread  = &mainthread; // thread courrant
static struct thread *nextthread = NULL;        // thread suivant schedulé


struct thread {
  unsigned int id;
  char isdone;
  void *retval;
  
  ucontext_t uc;
  LIST_ENTRY(thread) threads; // liste de threads
  
  int valgrind_stackid;
};


/* Generation structure de liste doublement chainée des threads */
LIST_HEAD(tqueue, thread) ready, done;



__attribute__((constructor))
static void __init()
{
	mainthread.id = 0;
	mainthread.isdone = 0;
	mainthread.retval = NULL;
	getcontext(&mainthread.uc);
	
	mainthread.state = THREAD_CANCEL_ENABLE;
	mainthread.type = THREAD_CANCEL_DEFERRED;
	
	LIST_INIT(&ready);
}


// Utiliser cette fonction pour éviter 1h de debugage à cause de curthread non
// mis à jour.
static int _swap_thread(struct thread *th1, struct thread *th2)
{
	int rv = 0;
	curthread = th2;

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
	return curthread;
}


int thread_create(thread_t *newthread, void *(*func)(void *), void *funcarg)
{
	static unsigned int id = 1;

	*newthread = malloc(sizeof (struct thread));

	if (*newthread == NULL) {
		perror("malloc");
		return -1;
	}

	getcontext(&(*newthread)->uc);
	(*newthread)->id = id++;
	(*newthread)->isdone = 0;
	(*newthread)->retval = NULL;

	(*newthread)->state = THREAD_CANCEL_ENABLE;
	(*newthread)->type = THREAD_CANCEL_DEFERRED;

	(*newthread)->uc.uc_link = &mainthread.uc;
	(*newthread)->uc.uc_stack.ss_size = 64*1024;

	(*newthread)->valgrind_stackid =
		VALGRIND_STACK_REGISTER(
			(*newthread)->uc.uc_stack.ss_sp,
			(*newthread)->uc.uc_stack.ss_sp
			+ (*newthread)->uc.uc_stack.ss_size
		);

  	(*newthread)->uc.uc_stack.ss_sp = malloc(64*1024);

	LIST_INSERT_HEAD(&ready, *newthread, threads);
  	makecontext(
		&(*newthread)->uc, (void (*)(void))_run, 3, *newthread, func, funcarg
	);

	return 0;
}


int thread_yield(void)
{
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

