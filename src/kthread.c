#define _GNU_SOURCE
#include <wait.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "kthread.h"

#define STACK_SIZE 1024*1024

int kthread_create(struct kthread *kt, void *(*func)(void *))
{
	void *stack, *stacktop;

	kt->mask = 0;

	if (NULL == (stack = malloc(STACK_SIZE))) {
		perror("malloc");
		return -1;
	}

	stacktop = stack + STACK_SIZE;
	kt->pid = clone(
		(int (*)(void *))func, stacktop, 
		CLONE_SIGHAND | CLONE_VM | CLONE_THREAD | 
		CLONE_FILES | CLONE_FS | CLONE_IO,
		"truc"
	);

	if (kt->pid == -1) {
		perror("clone");
		free(stack);
		return -1;
	}

	return 0;
}
