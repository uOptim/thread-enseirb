#include "thread.h"
#include "queue.h"

#include <ucontext.h>

static char init = 0;

static struct thread mainthread;
static struct thread *curthread  = &mainthread; // thread courrant
static struct thread *nextthread = &mainthread; // thread suivant schedulé

/* Generation structure de liste doublement chainée des threads */
LIST_HEAD(tqueue, thread) head;


struct thread {
	unsigned int id;
	ucontext_t uc;
	LIST_ENTRY(thread) threads; // liste de threads
};



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

	return 0;
}

int thread_yield(void)
{
	if (!init) __init();

	int rv = 0;
	struct thread *tmp;
	
	// yield depuis le main
	if (curthread == &mainthread) { 
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
		if (nextthread == NULL && !LIST_EMPTY(&head)) {
			// boucler si en fin de liste
			nextthread = LIST_FIRST(&head);
		}

		// donner la main au main
		tmp = curthread;
		curthread = &mainthread;
		
		// swapcontext
		rv = swapcontext(&tmp->uc, &curthread->uc);
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

