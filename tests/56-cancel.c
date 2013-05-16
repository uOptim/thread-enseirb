#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include "thread.h"

static void * f(void *_value)
{
  int i = 0;
  
  thread_setcancelstate(THREAD_CANCEL_DISABLE, NULL);
  
  for (i = 0; i < 20; i++) {
    printf("Je suis %ld (%d)\n", (long) _value, i);
    
    if (9 == i && NULL != _value) {
      thread_cancel((thread_t)_value);
    }

    if (17 == i && NULL == _value) {
      thread_setcancelstate(THREAD_CANCEL_ENABLE, NULL);
    }
    
    thread_yield();
  }

  return NULL;
}

int main(int argc, char *argv[])
{
  thread_t th1, th2;
  int err;
  void *res1 = NULL, *res2 = NULL;
  
  err = thread_create(&th1, f, NULL);
  assert(!err);
  err = thread_create(&th2, f, (void*) th1);
  assert(!err);

  err = thread_join(th2, &res2);
  assert(!err);
  
  return 0;
}
