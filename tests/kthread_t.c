#include "thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>


void *threadfunc(void *arg)
{
	thread_t t = thread_self();
	//thread_yield();
	fprintf(stderr, "vrai thread: %p\n", t);
	sleep(1);
	thread_exit(NULL);
}

int main(int argc, char **argv)
{
	void *ret;
	thread_t th1, th2, th3;
	
	thread_create(&th1, threadfunc, NULL);
	fprintf(stderr, "Added thread th1 = %p\n", th1);
	thread_create(&th2, threadfunc, NULL);
	fprintf(stderr, "Added thread th2 = %p\n", th2);
//	thread_create(&th3, threadfunc, NULL);
	thread_yield();
//	fprintf(stderr, "joining threads\n");
//	thread_join(th1, &ret);
//	fprintf(stderr, "th1 over\n");
//	thread_join(th2, &ret);
//	fprintf(stderr, "th2 over\n");
//	thread_join(th3, &ret);
//	fprintf(stderr, "th3 over\n");
	fprintf(stderr, "MAIN DONE\n");

	return 0;
}
