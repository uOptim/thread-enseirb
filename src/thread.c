#include "thread.h"
#include "queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>

#include <valgrind/valgrind.h>


static char init = 0;
static struct sigaction alarm_scheduler;
static sigset_t mask;
static struct thread mainthread;
static struct thread *scheduler;
static struct thread *curthread  = &mainthread; // thread courrant
static struct thread *nextthread = NULL;        // thread suivant schedulé

static struct thread *last_scheduled; 

static struct timeval tv;

struct thread {
	unsigned int id;
	char isdone;
	void *retval;

	ucontext_t uc;
	LIST_ENTRY(thread) threads; // liste de threads

	int valgrind_stackid;
};


/* Generation structure de liste doublement chainée des threads */
LIST_HEAD(tqueue, thread) ready, done;

static int _swap_thread(struct thread *th1, struct thread *th2);
static void _swap_scheduler();
static int _initialize_thread(thread_t *newthread, void* (*func)(void*), void *funcarg);
static void* _schedule_thread(void*);



static void __init()
{
	mainthread.id = 0;
	mainthread.isdone = 0;
	mainthread.retval = NULL;
	getcontext(&mainthread.uc);

	LIST_INIT(&ready);
	init = 1;

	scheduler = malloc(sizeof(struct thread));
	_initialize_thread(&scheduler, _schedule_thread, NULL);


	alarm_scheduler.sa_flags = SA_RESTART;
  	alarm_scheduler.sa_handler = &_swap_scheduler;
	//alarm_scheduler.sa_sigaction = NULL;
	sigfillset(&alarm_scheduler.sa_mask);

	if(sigaction(SIGALRM, &alarm_scheduler, NULL) == -1 ){
		perror("[ERROR] sigaction initialization");
		exit(2);
	}

	sigprocmask(0, NULL, &mask);
	
	// unblock sigalrm
	sigdelset(&mask, SIGALRM);
	ualarm(2000, 2000);
	sigsuspend(&mask);
}


static void* _schedule_thread(void* v){
  while (!LIST_EMPTY(&ready)){
    struct thread *self = thread_self();

    // on récupère le thread suivant
    nextthread = LIST_NEXT(last_scheduled, threads);
    
    /* 
     * Si on arrive à la fin de la file 
     * il faut boucler et revenir au début 
     */
    if (nextthread == NULL) {	
      printf("Je reviens au début de la liste %p \n", nextthread);
      nextthread = LIST_FIRST(&ready);
    }
    printf("Mon adresse %p et self %p \n", nextthread, self);
    
    
    gettimeofday(&tv, NULL);
    sigdelset(&mask, SIGALRM);

    // donner la main au thread suivant
    _swap_thread(self, nextthread);
    printf("debug\n");
  }
  return NULL;
}

static int _contains_one_element(){
  printf(" -- ici %p \n", LIST_FIRST(&ready));
  printf(" --- la %p \n", LIST_NEXT(LIST_FIRST(&ready),threads));
  return (LIST_NEXT(LIST_FIRST(&ready),threads) == NULL);
}


static void _swap_scheduler (int signal) {
  sigprocmask(0, NULL, &mask);
  /*  struct timeval tv2;
      gettimeofday(&tv2, NULL);
      unsigned long usec = (tv2.tv_sec-tv.tv_sec)*1000000 + (tv2.tv_usec-tv.tv_usec);*/
  struct thread *self = thread_self();
  //printf("thread %p: %ld usec\n", self, usec); 
  

  /* Si la liste est vide ou ne contient qu'un element
   * pas besoin de la préemption
   */
  if(LIST_EMPTY(&ready)){
    printf("Je suis viiiide \n");
    fflush(stdout);
    return;
  }
  else if(_contains_one_element()){
    printf("J'ai un seul element !! \n");
    fflush(stdout);
    return;
  }

  last_scheduled = self; 
  _swap_thread(self,scheduler);
}

// Utiliser cette fonction pour éviter 1h de debugage à cause de curthread non
// mis à jour.
static int _swap_thread(struct thread *th1, struct thread *th2)
{
	int rv = 0;
	curthread = th2;

	rv = swapcontext(&th1->uc, &th2->uc);

	if (rv) {
		perror("swapcontext");
	}

	return rv;
}


// sert à capturer la valeur de retour des threads ne faisant pas de
// thread_exit() mais un return
static void _run(struct thread *th, void *(*func)(void*), void *funcarg)
{
	void *retval;
	retval = func(funcarg);
	thread_exit(retval);
}


thread_t thread_self(void)
{
	if (!init) __init();

	return curthread;
}


static int _initialize_thread(thread_t *newthread, void *(*func)(void *), void *funcarg) {
	static unsigned int id = 1;

	if (!init) __init();

	*newthread = malloc(sizeof (struct thread));

	if (*newthread == NULL) {
		perror("malloc");
		return -1;
	}

	getcontext(&(*newthread)->uc);
	(*newthread)->id = id++;
	(*newthread)->isdone = 0;
	(*newthread)->retval = NULL;
	(*newthread)->uc.uc_link = &mainthread.uc;
	(*newthread)->uc.uc_stack.ss_size = 64*1024;

	(*newthread)->valgrind_stackid =
		VALGRIND_STACK_REGISTER(
			(*newthread)->uc.uc_stack.ss_sp,
			(*newthread)->uc.uc_stack.ss_sp
			+ (*newthread)->uc.uc_stack.ss_size
		);

  	(*newthread)->uc.uc_stack.ss_sp = malloc(64*1024);
	
  	makecontext(
		&(*newthread)->uc, (void (*)(void))_run, 3, *newthread, func, funcarg
	);

	return 0;
}

static void _insert_thread(thread_t newthread){
	LIST_INSERT_HEAD(&ready, newthread, threads);
}


int thread_create(thread_t *newthread, void *(*func)(void *), void *funcarg)
{
	_initialize_thread(newthread, func, funcarg);
	_insert_thread(*newthread);
	return 0;
}


int thread_yield(void)
{
	if (!init) __init();

	int rv = 0;
	struct thread *self = thread_self();
	
	// yield depuis le main
	if (self == &mainthread) { 
	        //cas où on arrive en bout de liste : on reboucle sur la tete
		if (nextthread == NULL && !LIST_EMPTY(&ready)) {
			nextthread = LIST_FIRST(&ready);
		}

		// swapcontext si thread schedulé
		if (nextthread != NULL) {
			rv = _swap_thread(self, nextthread);
		}
		
		// sinon ne rien faire (rester dans main)
	} 

	// yield depuis un thread != du main
	else { 
		// màj thread suivant
		nextthread = LIST_NEXT(self, threads);
		// donner la main au mainthread
		rv = _swap_thread(self, &mainthread);
	}

	return rv;
}


int thread_join(thread_t thread, void **retval)
{
	if (!init) __init();

	int rv = 0;

	while (!thread->isdone) {
		rv = thread_yield();
	}

	*retval = thread->retval;

	if (thread != &mainthread) {
		// libérer ressource
		VALGRIND_STACK_DEREGISTER(thread->valgrind_stackid);
		free(thread->uc.uc_stack.ss_sp);
		free(thread);
	}

	return rv;
}


void thread_exit(void *retval)
{
	if (!init) __init();

	struct thread *self = thread_self();

	self->isdone = 1;
	self->retval = retval;

	if (self != &mainthread) {
		// màj thread suivant
		nextthread = LIST_NEXT(self, threads);
		LIST_REMOVE(self, threads);

		// repasser au main
		_swap_thread(self, &mainthread);
	}

	else {
		do {
			thread_yield();
		} while (!LIST_EMPTY(&ready));
	}

	exit(0);
}

