#include "thread.h"
#include "queue.h"

#include <ucontext.h>

static char init = 0;

static struct thread mainthread;
static struct thread *cur = &mainthread; // thread courrant

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
	LIST_INSERT_HEAD(&head, &mainthread, threads);

	init = 1;
}

thread_t thread_self(void)
{
	if (!init) __init();

	return cur;
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
	
	tmp = cur;
	cur = LIST_NEXT(cur, threads);

	if (cur == NULL) {
		cur = tmp;
	} else {
		rv = swapcontext(&tmp->uc, &cur->uc);
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

