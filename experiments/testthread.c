#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

#define STACK_SIZE 1024*1024

int func(void * arg)
{
	pid_t pid = getpid();
	printf("Plop! Thread here! PID = %d\n", pid);
	printf("Got arg: %s\n", (char *) arg);
	return 0;
}

int main (int argc, char **argv)
{
	pid_t pid;
	void *stack, *stacktop;

	pid = getpid();
	printf("Parent here PID = %d\n", pid);

	if (NULL == (stack = malloc(STACK_SIZE))) {
		perror("malloc");
		return -1;
	}

	stacktop = stack + STACK_SIZE;
	pid = clone(
		&func, stacktop, CLONE_SIGHAND | CLONE_VM | CLONE_THREAD, "truc"
	);

	if (pid == -1) {
		perror("clone");
		return -1;
	}

	sleep(1);

	return 0;
}
