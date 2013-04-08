#include <pthread.h>
#include <stdio.h>
#include <assert.h>

static void * threadfunc(void * arg)
{
  char *name = arg;
  printf("je suis le thread %p, lancé avec l'argument %s\n",
	 (void*) pthread_self(), name);
  sched_yield();
  printf("je suis encore le thread %p, lancé avec l'argument %s\n",
	 (void *) pthread_self(), name);
  pthread_exit(arg);
}

int main(int argc, char *argv[])
{
  pthread_t thread1, thread2;
  void *retval1, *retval2;
  int err;

  printf("le main lance 2 threads...\n");
  err = pthread_create(&thread1, NULL, threadfunc, "thread1");
  assert(!err);
  err = pthread_create(&thread2, NULL, threadfunc, "thread2");
  assert(!err);
  printf("le main a lancé les threads %p et %p\n",
	 (void *) thread1, (void *) thread2);

  printf("le main attend les threads\n");
  err = pthread_join(thread2, &retval2);
  assert(!err);
  err = pthread_join(thread1, &retval1);
  assert(!err);
  printf("les threads ont terminé en renvoyant '%s' and '%s'\n",
	 (char *) retval1, (char *) retval2);

  return 0;
}
