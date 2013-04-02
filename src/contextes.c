#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h> /* ne compile pas avec -std=c89 ou -std=c99 */

void func(int numero)
{
  printf("j'affiche le numéro %d\n", numero);
}

int main() {
  ucontext_t uc, previous;

  getcontext(&uc); /* initialisation de uc avec valeurs coherentes
		    * (pour éviter de tout remplit a la main ci-dessous) */

  uc.uc_stack.ss_size = 64*1024;
  uc.uc_stack.ss_sp = malloc(uc.uc_stack.ss_size);
  uc.uc_link = &previous;
  makecontext(&uc, (void (*)(void)) func, 1, 34);

  printf("je suis dans le main\n");
  swapcontext(&previous, &uc);
  printf("je suis revenu dans le main\n");

  uc.uc_stack.ss_size = 64*1024;
  uc.uc_stack.ss_sp = malloc(uc.uc_stack.ss_size);
  uc.uc_link = NULL;
  makecontext(&uc, (void (*)(void)) func, 1, 57);

  printf("je suis dans le main\n");
  setcontext(&uc);
  printf("je ne reviens jamais ici\n");
  return 0;
}
