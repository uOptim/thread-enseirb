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

TYPE *T;

static void print(TYPE length) {
  if(length > 100)
    return;

  int i;
  
  fprintf(stdout, "T = [%d", T[0]);
  for (i = 1; i < length; i++) {
    fprintf(stdout, " %d", T[i]);
  }
  fprintf(stdout, "]\n");
}

static TYPE partitionner(TYPE premier, TYPE dernier, TYPE pivot) {
  TYPE i, j = T[pivot], tmp;
  T[pivot] = T[dernier];
  T[dernier] = j;
  
  j = premier;

  for (i = premier; i < dernier; i++) {
    if (T[i] <= T[dernier]) {
      tmp = T[i];
      T[i] = T[j];
      T[j] = tmp;
      
      j++;
    }
  }
  tmp = T[dernier];
  T[dernier] = T[j];
  T[j] = tmp;
  
  return j;
}

static void * triRapide(void *_value)
{
  thread_t th, th2;
  int err;
  void *res = NULL, *res2 = NULL;
  TYPE premier = ((TYPE *) _value)[0];
  TYPE dernier = ((TYPE *) _value)[1];

  if (premier >= dernier)
    return NULL;
  
  TYPE pivot = premier + rand()%(dernier - premier + 1);
  pivot = partitionner(premier, dernier, pivot);
  
  TYPE value1[2] = {premier, pivot - 1};
  TYPE value2[2] = {pivot + 1, dernier};
  
  err = thread_create(&th, triRapide, (void*)value1);
  assert(!err);
  err = thread_create(&th2, triRapide, (void*)value2);
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
  T = malloc(length*sizeof(int));
  
  srand(time(NULL));
  
  for (i = 0; i < length; i++) {
    T[i] = rand() % 100;
  }
  
  print(length);
  TYPE value[2] = {0, length - 1};
  triRapide((void *)value);
  print(length);

  return 0;
}
