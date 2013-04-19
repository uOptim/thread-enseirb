#define _GNU_SOURCE
#include <wait.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "kthread.h"

#define STACK_SIZE 1024*1024


int kthread_create(struct kthread *kt, int (*func)(void *), void *arg)
{
	void *stack;

	kt->mask = 0;

	if (NULL == (stack = malloc(STACK_SIZE))) {
		perror("malloc");
		return -1;
	}

	kt->pid = clone(
		func, stack + STACK_SIZE, 
		CLONE_SIGHAND | CLONE_VM | CLONE_THREAD | 
		CLONE_FILES | CLONE_FS | CLONE_IO,
		arg
	);

	if (kt->pid == -1) {
		perror("clone");
		free(stack);
		return -1;
	}

	return 0;
}
