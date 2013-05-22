#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include "pthread.h"

/* test de plein de create-destroy consécutifs.
 *
 * valgrind doit etre content.
 * la durée du programme doit etre proportionnelle au nombre de pthreads donnés en argument.
 * jusqu'à combien de pthreads cela fonctionne-t-il ?
 *
 * support nécessaire:
 * - pthread_create()
 * - pthread_exit()
 * - pthread_join() avec récupération de la valeur de retour
 */

static void * thfunc(void *dummy)
{
  pthread_exit(NULL);
}

int main(int argc, char *argv[])
{
  pthread_t th;
  int err, i, nb;
  void *res;

  if (argc < 2) {
    printf("argument manquant: nombre de pthreads\n");
    return -1;
  }

  nb = atoi(argv[1]);

  for(i=0; i<nb; i++) {
    err = pthread_create(&th, NULL, thfunc, NULL);
    assert(!err);
    err = pthread_join(th, &res);
    assert(!err);
    assert(res == NULL);
  }

  printf("%d pthreads créés et détruits\n", nb);
  return 0;
}
