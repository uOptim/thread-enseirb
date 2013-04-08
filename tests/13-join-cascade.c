#include <stdio.h>
#include <assert.h>
#include "thread.h"

/* test du join du main par un fils lui même join par un second fils.
 *
 * support nécessaire:
 * - thread_create()
 * - thread_self() dans le main
 * - thread_exit() dans le main
 * - thread_join() du main par un autre thread
 */

thread_t thmain = NULL;

static void * thfunc(void *dummy)
{
	void *res;
	int err;

	err = thread_join(thmain, &res);
	assert(!err);
	assert(res == (void*) 0xdeadbeef);
	printf("main terminé OK\n");
	return (void *) 0xdeadbeef;
}

static void * thfunc2(void *arg) 
{
	int err;
	void *res;

	err = thread_join((thread_t) arg, &res);
	assert(!err);
	assert(res == (void *) 0xdeadbeef);
	printf("thread sub terminé OK\n");
	return NULL;
}

int main()
{
	int err;
	thread_t th1, th2;

	thmain = thread_self();

	err = thread_create(&th1, thfunc, NULL);
	assert(!err);
	err = thread_create(&th2, thfunc2, th1);
	assert(!err);

	thread_exit((void*) 0xdeadbeef);
}
