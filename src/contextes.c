#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h> /* ne compile pas avec -std=c89 ou -std=c99 */

void func(int numero)
{
  printf("j'affiche le numéro %d\n", numero);
}

int main() {
  ucontext_t uc1, uc2, uc3, previous;

  getcontext(&uc1);
  getcontext(&uc2);
  getcontext(&uc3);

  uc1.uc_stack.ss_size = 64*1024;
  uc1.uc_stack.ss_sp = malloc(uc1.uc_stack.ss_size);
  uc1.uc_link = NULL;
  makecontext(&uc1, (void (*)(void)) func, 1, 34);

  uc2.uc_stack.ss_size = 64*1024;
  uc2.uc_stack.ss_sp = malloc(uc2.uc_stack.ss_size);
  uc2.uc_link = &uc1;
  makecontext(&uc2, (void (*)(void)) func, 1, 57);

  uc3.uc_stack.ss_size = 64*1024;
  uc3.uc_stack.ss_sp = malloc(uc3.uc_stack.ss_size);
  uc3.uc_link = &uc2;
  makecontext(&uc3, (void (*)(void)) func, 1, 67);

  printf("je suis dans le main\n");
  swapcontext(&previous, &uc3);
  printf("je suis revenu dans le main\n");

  printf("Quit depuis le main\n");
  return 0;
}
