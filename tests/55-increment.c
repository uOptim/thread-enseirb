#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "thread.h"

static unsigned long max;
static void * increment(void *_value) {
  unsigned long value = (unsigned long) _value;
  while (value < max) value++;
  return (void*)(value);
}

int main(int argc, char *argv[])
{
  if (argc < 2) {
    printf("Argument manquant.\nusage : %s max_value\n", argv[0]);
    return -1;
  }
  max = atoi(argv[1]);
  thread_t th, th2, th3, th4;
  void * res = NULL, *res2 = NULL, *res3 = NULL, *res4 = NULL;
  int err;
  unsigned long value = 0;

  err = thread_create(&th, increment, (void*)(value));
  assert(!err);
  err = thread_create(&th2, increment, (void*)(value));
  assert(!err);
  err = thread_create(&th3, increment, (void*)(value));
  assert(!err);
  err = thread_create(&th4, increment, (void*)(value));
  assert(!err);

  err = thread_join(th, &res);
  assert(!err);
  err = thread_join(th2, &res2);
  assert(!err);
  err = thread_join(th3, &res3);
  assert(!err);
  err = thread_join(th4, &res4);
  assert(!err);

  printf("TH1 -> %ld\nTH2 -> %ld\nTH3 -> %ld\nTH4 -> %ld\n",
	 (unsigned long) res,  (unsigned long) res2,
	 (unsigned long) res3,  (unsigned long) res4);
 
  return 0;
}
