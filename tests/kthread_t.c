#include "thread.h"
#include "kthread.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* TMP */
#include <pthread.h>
pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
/* TMP END */

void *threadfunc(void *arg)
{
	fprintf(stderr, "vrai thread!\n");
	thread_exit(NULL);
}

int main(int argc, char **argv)
{
	//thread_self();

	thread_t thread;
	thread_create(&thread, threadfunc, NULL);

	sleep(1);

	void *ret;
	thread_join(thread, &ret);

	fprintf(stderr, "MAIN DONE\n");

	return 0;
}
