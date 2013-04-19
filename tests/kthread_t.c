#include "thread.h"
#include "kthread.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>


void *threadfunc(void *arg)
{
	fprintf(stderr, "vrai thread!\n");
	return NULL;
}

int func1(void *arg)
{
	void *ret;
	thread_t thread;

	thread_create(&thread, threadfunc, NULL);
	thread_join(thread, &ret);

	return 0;
}


int main(int argc, char **argv)
{
	int nb_threads = 1;

	if (argc > 1) {
		nb_threads = atoi(argv[1]);
		nb_threads = (nb_threads > 0) ? nb_threads : 1;
	}

	int i;
	struct kthread *kt;
	for (i = 0; i < nb_threads; i++) {
		kt = malloc(sizeof *kt);
		kthread_create(kt, func1, NULL);
	}

	sleep(1);

	fprintf(stderr, "MAIN DONE\n");

	return 0;
}
