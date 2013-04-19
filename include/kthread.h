#ifndef _KTHREAD_H_
#define _KTHREAD_H_

#include <ucontext.h>

#define KTHREAD_CANCEL 0x01

struct kthread {
	char mask;
	pid_t pid;
	ucontext_t *ucp;
};

int kthread_create(struct kthread *, int (*)(void *), void *);

#endif
