#include "thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>


void *threadfunc(void *arg)
{
	thread_t t = thread_self();
	thread_yield();
	fprintf(stderr, "vrai thread: %p\n", t);
	return NULL;
}

int main(int argc, char **argv)
{
	thread_t thread;
	
	thread_create(&thread, threadfunc, NULL);
	sleep(1);
	fprintf(stderr, "MAIN DONE\n");

	return 0;
}
