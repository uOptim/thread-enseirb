#include "thread.h"
#include "queue.h"

#include <stdlib.h>
#include <ucontext.h>

static char init = 0;

static struct thread mainthread;
static struct thread *curthread  = &mainthread; // thread courrant
static struct thread *nextthread = NULL;        // thread suivant schedulé


struct thread {
	unsigned int id;
	ucontext_t uc;
	LIST_ENTRY(thread) threads; // liste de threads
};


/* Generation structure de liste doublement chainée des threads */
LIST_HEAD(tqueue, thread) head;



static void __init()
{
	mainthread.id = 0;
	getcontext(&mainthread.uc);

	LIST_INIT(&head);
	// mainthread semble être un cas particulier et n'est pas traîté de la
	// même façon que les autres threads d'après tests/02-switch.c
	//LIST_INSERT_HEAD(&head, &mainthread, threads);

	init = 1;
}


thread_t thread_self(void)
{
	if (!init) __init();

	return curthread;
}


int thread_create(thread_t *newthread, void *(*func)(void *), void *funcarg)
{
	static unsigned int id = 1;

	if (!init) __init();

	*newthread = malloc(sizeof (struct thread));

	if (*newthread == NULL) {
		perror("malloc");
		return -1;
	}

	fprintf(stderr, "Creating new thread, id = %d\n", id);

	(*newthread)->id = id++;
	getcontext(&(*newthread)->uc);
	(*newthread)->uc.uc_link = &mainthread.uc;
	(*newthread)->uc.uc_stack.ss_size = 64*1024;
  	(*newthread)->uc.uc_stack.ss_sp = malloc(64*1024);
  	makecontext(&(*newthread)->uc, (void (*)(void)) func, 1, funcarg);
	LIST_INSERT_HEAD(&head, *newthread, threads);

	return 0;
}


int thread_yield(void)
{
	if (!init) __init();

	int rv = 0;
	struct thread *tmp;
	
	// yield depuis le main
	if (curthread == &mainthread) { 
		if (nextthread == NULL && !LIST_EMPTY(&head)) {
			nextthread = LIST_FIRST(&head);
		}

		// swapcontext si thread schedulé
		if (nextthread != NULL) {
			curthread = nextthread;
			rv = swapcontext(&mainthread.uc, &curthread->uc);
		}
	} 

	// yield depuis un thread != du main
	else { 
		// màj thread suivant
		nextthread = LIST_NEXT(curthread, threads);

		// donner la main au mainthread
		tmp = curthread;
		curthread = &mainthread;
		rv = swapcontext(&tmp->uc, &mainthread->uc);
	}

	return rv;
}


int thread_join(thread_t thread, void **retval)
{
	if (!init) __init();

	return 0;
}


void thread_exit(void *retval)
{
	if (!init) __init();
}

