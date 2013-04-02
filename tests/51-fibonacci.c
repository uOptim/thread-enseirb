#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "thread.h"

/* fibonacci.
 *
 * la durée doit être proportionnel à la valeur du résultat.
 * valgrind doit être content.
 * jusqu'à quelle valeur cela fonctionne-t-il ?
 *
 * support nécessaire:
 * - thread_create()
 * - thread_join() avec récupération de la valeur de retour
 * - retour sans thread_exit()
 */

static void * fibo(void *_value)
{
  thread_t th, th2;
  int err;
  void *res = NULL, *res2 = NULL;
  unsigned long value = (unsigned long) _value;

  if (value < 3)
    return (void*) 1;

  err = thread_create(&th, fibo, (void*)(value-1));
  assert(!err);
  err = thread_create(&th2, fibo, (void*)(value-2));
  assert(!err);

  err = thread_join(th, &res);
  assert(!err);
  err = thread_join(th2, &res2);
  assert(!err);

  return (void*)((unsigned long) res + (unsigned long) res2);
}

int main(int argc, char *argv[])
{
  unsigned long value, res;

  if (argc < 2) {
    printf("argument manquant: entier x pour lequel calculer fibonacci(x)\n");
    return -1;
  }

  value = atoi(argv[1]);
  res = (unsigned long) fibo((void *)value);
  printf("fibo de %ld = %ld\n", value, res);

  return 0;
}
