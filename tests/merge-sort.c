#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "thread.h"

#define NOTHREADS 2
#define NB_ELEMENTS 9

int a[] = {10, 8, 5, 2, 3, 6, 7, 1, 4, 9};

typedef struct node {
  int begin;
  int end;
} NODE;

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

  n1.begin = p->begin;
  n1.end = mid;

  n2.begin = mid+1;
  n2.end = p->end;

  if (p->begin >= p->end) return;

  ret = thread_create(&tid1, mergesort, (void*)(&n1));
  assert(!ret);
  ret = thread_create(&tid2,  mergesort, (void*)(&n2));
  assert(!ret);

  printf("coucou");
  fflush(stdout);
  ret = thread_join(tid1, NULL);
  assert(!ret);
  ret = thread_join(tid2, NULL);
  assert(!ret);


  merge(p->begin, p->end);
  thread_exit(NULL);
  return NULL;
}


int main()
{
  int i;
  NODE m;
  m.begin = 0;
  m.end = NB_ELEMENTS;
  thread_t tid;

  int ret; 

  ret=thread_create(&tid, mergesort, &m);
  if (ret) {
    printf("%d %s - unable to create thread - ret - %d\n", __LINE__, __FUNCTION__, ret);    
    exit(1);
  }

  thread_join(tid, NULL);

  for (i = 0; i < 10; i++)
    printf ("%d ", a[i]);

  printf ("\n");

  //thread_exit(NULL);
  return 0;
}
