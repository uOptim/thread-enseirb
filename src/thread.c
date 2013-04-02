#include "thread.h"

thread_t thread_self(void)
{
	thread_t tmp = (void *) 0;

	return tmp;
}

int thread_create(thread_t *newthread, void *(*func)(void *), void *funcarg)
{
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

void thread_exit(void *retval)
{
}

