#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include "thread.h"

#define NOTHREADS 2

int *a;

typedef struct node {
  int begin;
  int end;
} NODE;


int rand_a_b(int a, int b){
    return rand()%(b-a) +a;
}

void merge(int i, int j)
{
  int mid = (i+j)/2;
  int ai = i;
  int bi = mid+1;

  int newa[j-i+1], newai = 0;

  while(ai <= mid && bi <= j) {
    if (a[ai] > a[bi])
      newa[newai++] = a[bi++];
    else                    
      newa[newai++] = a[ai++];
  }

  while(ai <= mid) {
    newa[newai++] = a[ai++];
  }

  while(bi <= j) {
    newa[newai++] = a[bi++];
  }

  for (ai = 0; ai < (j-i+1) ; ai++)
    a[i+ai] = newa[ai];

}

void * mergesort(void *a)
{
  NODE *p = (NODE *)a;
  NODE n1, n2;
  int mid = (p->begin+p->end)/2;
  thread_t tid1, tid2;
  int ret;
  void *res1,*res2;

  n1.begin = p->begin;
  n1.end = mid;

  n2.begin = mid+1;
  n2.end = p->end;

  if (p->begin >= p->end) return NULL;

  ret = thread_create(&tid1, mergesort, (void*)(&n1));
  assert(!ret);
  ret = thread_create(&tid2,  mergesort, (void*)(&n2));
  assert(!ret);

  ret = thread_join(tid1, &res1);
  assert(!ret);

  ret = thread_join(tid2, &res2);
  assert(!ret);

  merge(p->begin, p->end);
  thread_exit(NULL);
  return NULL;
}


int main(int argc, char * argv[])
{

  if (argc < 2) {
    printf("argument manquant: nombre de cases du tableau\n");
    return -1;
  }


  int nb_elements = atoi(argv[1]);
  /* mx value d'un int */
  if(nb_elements <= 0 && nb_elements > 4294967296)
    {
      printf("valeur de l'argument invalide\n");
      return -1;
    }

  int i;
  NODE m;
  m.begin = 0;
  m.end = nb_elements-1;
  
  thread_t tid;
  void *res;
  int ret; 

  a = malloc(nb_elements*sizeof(int));

  srand(time(NULL)); 
  for (i = 0; i < nb_elements; i++)
    a[i] = rand_a_b(0,nb_elements);

  ret=thread_create(&tid, mergesort, &m);
  assert(!ret);

  ret = thread_join(tid, &res);
  assert(!ret);

  for (i = 0; i < nb_elements; i++)
    printf ("%d ", a[i]);

  printf ("\n");

  free(a);
  
  return 0;
}
