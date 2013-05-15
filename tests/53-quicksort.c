#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include "thread.h"

/* Tri Rapide.
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

#define TYPE int

// Define LRANGE <= i < HRANGE
#define LRANGE 0
#define HRANGE 100

TYPE *T;

static void print(TYPE length) {
  if(length > 100)
    return;

  TYPE i;
  
  fprintf(stdout, "T = [%d", T[0]);
  for (i = 1; i < length; i++) {
    fprintf(stdout, " %d", T[i]);
  }
  fprintf(stdout, "]\n");
}

static TYPE partition(TYPE first, TYPE last, TYPE pivot) {
  TYPE i, j = T[pivot], tmp;
  T[pivot] = T[last];
  T[last] = j;
  
  j = first;

  for (i = first; i < last; i++) {
    if (T[i] <= T[last]) {
      tmp = T[i];
      T[i] = T[j];
      T[j] = tmp;
      
      j++;
    }
  }
  tmp = T[last];
  T[last] = T[j];
  T[j] = tmp;
  
  return j;
}

static void * quicksort(void *_value)
{
  thread_t th, th2;
  int err;
  void *res = NULL, *res2 = NULL;
  TYPE first = ((TYPE *) _value)[0];
  TYPE last = ((TYPE *) _value)[1];

  if (first >= last)
    return NULL;
  
  TYPE pivot = first + rand()%(last - first + 1);
  pivot = partition(first, last, pivot);
  
  TYPE value1[2] = {first, pivot - 1};
  TYPE value2[2] = {pivot + 1, last};
  
  err = thread_create(&th, quicksort, (void*)value1);
  assert(!err);
  err = thread_create(&th2, quicksort, (void*)value2);
  assert(!err);

  err = thread_join(th, &res);
  assert(!err);
  err = thread_join(th2, &res2);
  assert(!err);

  return NULL;
}

int main(int argc, char *argv[])
{
  TYPE length, i;
  
  if (argc < 2) {
    printf("argument manquant: entier n donnant la taille du tableau\n");
    return -1;
  }
  
  length = atoi(argv[1]);
  T = malloc(length*sizeof(TYPE));
  
  srand(time(NULL));
  
  for (i = 0; i < length; i++) {
    T[i] = LRANGE + rand()%(HRANGE - LRANGE);
  }
  
  print(length);
  TYPE value[2] = {0, length - 1};
  quicksort((void *)value);
  print(length);
  
  free(T);
  
  return 0;
}
