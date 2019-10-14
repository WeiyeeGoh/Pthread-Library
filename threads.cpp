#include <iostream>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <setjmp.h>
#include <sys/time.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <semaphore.h>
using namespace std;

struct TCB;
struct Semaphore;

static int id_counter = 0;
static int thread_count = 0;
static bool delete_thread_flag = false;
static int cur_t_ind = -1;		//-1 because nothing in it rn. Becomes 0 when first thread is made. Then it changes as we round robin schedule
static vector<TCB*> tcb_arr;
static struct itimerval new_val;
static struct itimerval null_time;
static struct sigaction sa;
static sigset_t signal_set;
static vector<Semaphore*> sem_arr;
static int sem_counter = 0;

//Include pointer to heap so I can delete space easy

struct TCB {
	void* heap_ptr;
	pthread_t num;
	jmp_buf buf;
	int status;	//0 is exit, 1 is ready, 2 is running
	void *(*start_routine)(void*);
	void *arg;
	void *return_value;
	TCB* waited_by;
};

struct Semaphore {
	unsigned int value;
	vector<TCB*> blocked_threads;
	bool blocking;
	unsigned int id;
};

static TCB gb_collect;

static long int i64_ptr_mangle(long int p){
	long int ret;
	asm(" mov %1, %%rax;\n"
		" xor %%fs:0x30, %%rax;"
		" rol $0x11, %%rax;"
		" mov %%rax, %0;"
	: "=r"(ret)
	: "r"(p)
	: "%rax"
	);
	return ret;
}

void lock() {
	sigemptyset(&signal_set);
	sigaddset(&signal_set, SIGALRM);
	sigprocmask(SIG_BLOCK, &signal_set, NULL);
}

void unlock() {
	sigemptyset(&signal_set);
	sigaddset(&signal_set, SIGALRM);
	sigprocmask(SIG_UNBLOCK, &signal_set, NULL);
}


//TODO: running status stuff
void schedule() {
	lock();

	//Use to have a check before. Now it doesn't matter becz we dont care about current one. just care about finding the next thread
	do {
		cur_t_ind++;
		if(cur_t_ind > tcb_arr.size()-1) {
			cur_t_ind = 0;
		}
	} while(tcb_arr[cur_t_ind]->status == 0 || tcb_arr[cur_t_ind]->status == 3);

	TCB* next_thread = tcb_arr[cur_t_ind];
	unlock();

	longjmp(next_thread->buf, 1);
	
}

void garbage_collection() {
	while(true) {

	// 	void *(*start_routine)(void*);
	// void *arg;
		free(tcb_arr[cur_t_ind]->heap_ptr);
		//free(tcb_arr[cur_t_ind]->arg);
		if(!setjmp(gb_collect.buf)) {
			unlock();
			schedule();
		}
	}

}

//TO'DO:Change up sig_handler to call schedule
void sig_handler(int signal) {
	//setitimer(ITIMER_REAL, &null_time, NULL);
	//cout << "hello" << endl;
	int cur_temp_ind = cur_t_ind;
	if(tcb_arr.size() > 0) {
		if(!setjmp(tcb_arr[cur_t_ind]->buf)) {
			schedule();
		}
	}
	cur_t_ind = cur_temp_ind;
}

int init() {

	if(tcb_arr.size() == 0) {
	//First instance is for sure a main
		TCB* main_tcb = new TCB;

		if(!setjmp(main_tcb->buf)) {
			//initiate our thing
			ualarm(50000,50000);

			//set up main as a thread. create tcb and push to arr
			main_tcb->num = id_counter;
			id_counter++;
			main_tcb->status = 1;
			main_tcb->start_routine = NULL;
			main_tcb->arg = NULL;
			main_tcb->waited_by = NULL;
			main_tcb->return_value = NULL;
			tcb_arr.push_back(main_tcb);
			cur_t_ind = 0;

			sa.sa_handler = sig_handler;
			sa.sa_flags = SA_NODEFER;

			if(sigaction(SIGALRM, &sa, NULL) == -1) {
				perror("Sigaction failed");
			}

			//Set up our garbage collector
			gb_collect.num = 1;
			gb_collect.status = 1;
			long int* thread_stack = (long int*)malloc(32768);
			memset(thread_stack, 0, 32768);
			void* top_of_stack = thread_stack + 32768/8;

			setjmp(gb_collect.buf);
			*(((long int*) &gb_collect.buf)+6) = i64_ptr_mangle((long int)((long int*)top_of_stack-2));
			*(((long int*) &gb_collect.buf)+7) = i64_ptr_mangle((long int)(garbage_collection));
			gb_collect.heap_ptr = thread_stack;
			gb_collect.start_routine = NULL;
			gb_collect.arg = NULL;
			gb_collect.waited_by = NULL;
			gb_collect.return_value = NULL;
		}
	}
}

int pthread_join(pthread_t thread, void **value_ptr) {
	//CHeck error 
	//Find tcp with thread id
	if(tcb_arr.size() == 0) {
		init();
	}

	TCB* cur_tcb = tcb_arr[cur_t_ind];

	if (cur_tcb->num == thread) {
		return EDEADLK;
	}

	TCB* wait_thread = NULL;
	lock();
	for(int i = 0; i < tcb_arr.size(); i++) {
		if (tcb_arr[i]->num == thread) {
			wait_thread = tcb_arr[i];
			break;
		}
	}
	unlock();

	if (wait_thread == NULL) {
		return ESRCH;
	}
	//TODO: Might have to deal with indexes being messed up because cur_index is not reliable. Save a local pointer to that tcb

	if (wait_thread->waited_by != NULL) {
		return EINVAL;
	}

	//check if status is 0
	//if not 0, then just schedule
	if(wait_thread->status != 0) {
		//TODO: Might be an issue here if no lock. Maybe another thread becomes waited_by right after checking there was none
		lock();
		if (wait_thread->waited_by != NULL) {	
			return EINVAL;
		}
		cur_tcb->status = 3;	//3 represents blocked
		wait_thread->waited_by = cur_tcb;
		unlock();		
		if(!setjmp(cur_tcb->buf)) {
			schedule();
		}
	}
	//put return value of pthread_exit into value_ptr
	if(value_ptr != NULL) {
		*value_ptr = wait_thread->return_value;
	}

	int t_index = 0;
	lock();
	for(int i = 0; i < tcb_arr.size(); i++) {
		if (tcb_arr[i]->num == thread) {
			t_index = i;
			break;
		}
	}
	tcb_arr.erase(tcb_arr.begin()+t_index);
	unlock();

	return 0;
}

int wrapper_func() {
	void *(*start_routine)(void*) = tcb_arr[cur_t_ind]->start_routine;
	void *arg = tcb_arr[cur_t_ind]->arg;
	//TO'DO: run start routine with arg;
	
	pthread_exit(start_routine(arg));
}	

//Garbage Collection
//While loop infinitely
	//Delete only pointer stuff. So Heap ptr, function ptr, and args. Only really need pid and status code
		//Could delete whole TCB and then just save it in another array. 
		//Other option is to leave it in TCB array and while loop to skip over exited stuff
	//Remove lock
	//Schedule


//TODO: mem leak check
int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void*), void *arg) {
	
	//Create our main buff. We always want to setjmp here because we will be calling scheduler for sure. 
	//NOTE: maybe switch out how i use stuff

	//First instance is for sure a main
	if(tcb_arr.size() == 0) {
		init();
	}



	if(!setjmp(tcb_arr[cur_t_ind]->buf)) {
		//malloc space for our stack in heap
		long int* thread_stack = (long int*)malloc(32768);
		memset(thread_stack, 0, 32768);
		void* top_of_stack = thread_stack + 32768/8;

		TCB* t1 = new TCB;

		//Setting jmp_buf for new thread
		//TO'DO: put function inside a wrapper also
		setjmp(t1->buf);
		*(((long int*) &(t1->buf))+6) = i64_ptr_mangle((long int)((long int*)top_of_stack-2));
		*(((long int*) &(t1->buf))+7) = i64_ptr_mangle((long int)(wrapper_func));

		//Create TCB to hold data
		t1->heap_ptr = thread_stack;
		t1->num = id_counter;
		id_counter++;
		t1->status = 1;
		t1->start_routine = start_routine;
		t1->arg = arg;
		t1->waited_by = NULL;
		t1->return_value = NULL;
		*thread = t1->num;

		tcb_arr.push_back(t1);
		//schedule();
	}

	//TO'DO: change to tid of newly created thread
	//setitimer(ITIMER_REAL, &new_val, NULL);

	return 0;
		
}

// void garbage_collection() {
// 	while(true) {

// 	// 	void *(*start_routine)(void*);
// 	// void *arg;
// 		free(tcb_arr[cur_t_ind]->heap_ptr);
// 		//free(tcb_arr[cur_t_ind]->arg);
// 		free(tcb_arr[cur_t_ind]);
// 		tcb_arr.erase(tcb_arr.begin()+cur_t_ind);
// 		unlock();
// 		if(!setjmp(gb_collect.buf)) {
// 			schedule();
// 		}
// 	}

// }


//TO'DO: reduce thread_count everytime I kill a thread
//TO'DO: end thread system when I kill down to 1 thread left(main thread). Basically reset everything. 
void pthread_exit(void *retval) {
	lock();
	TCB* cur_tcb = tcb_arr[cur_t_ind];

	cur_tcb->status = 0; //sets this thread's status to exit. 
	cur_tcb->return_value = retval;
	//TODO:Jump to garbage collector and delete stack pointer and other relavent info

	if(cur_tcb->waited_by != NULL) {
		cur_tcb->waited_by->status = 1;
	}
	
	longjmp(gb_collect.buf, 1);
	
}

pthread_t pthread_self(void) {
	return tcb_arr[cur_t_ind]->num;
}

int sem_init(sem_t *sem, int pshared, unsigned int value) {
	if(tcb_arr.size() == 0) {
		init();
	}
	if(value > 65536) {
		return EINVAL;
	}
	if(pshared != 0) {
		return ENOSYS;
	}


	lock();
	Semaphore *my_sem = new Semaphore();
	my_sem->value = value;
	my_sem->id = sem_counter;
	sem_counter++;
	my_sem->blocking = false;
	sem_arr.push_back(my_sem);

	sem->__align = (long int)my_sem;
	unlock();
	return 0;
}
int sem_destroy(sem_t *sem) {
	if(tcb_arr.size() == 0) {
		init();
	}

	lock();
	Semaphore *my_sem = (Semaphore*)(sem->__align);

	for(int i = 0; i < my_sem->blocked_threads.size(); i++) {
		free(my_sem->blocked_threads[i]);
	}

	for(int i = 0; i < sem_arr.size(); i++) {
		if(sem_arr[i]->id == my_sem->id) {
			sem_arr.erase(sem_arr.begin() + i);
		}
	}

	free(my_sem);


	unlock();

	return 0;
}
int sem_wait(sem_t *sem) {
	if(tcb_arr.size() == 0) {
		init();
	}

	lock();

	Semaphore *my_sem = (Semaphore*)(sem->__align);
	//Check if it is a valid semaphore maybe?
	if(my_sem->value == 0) {
		if(!setjmp(tcb_arr[cur_t_ind]->buf)) {
			tcb_arr[cur_t_ind]->status = 3;
			my_sem->blocked_threads.push_back(tcb_arr[cur_t_ind]);
			my_sem->blocking = true;
			unlock();
			schedule();
		}
	} else {
		unlock();
	}

	lock();

	for(int i = 0; i < my_sem->blocked_threads.size(); i++) {
		if(my_sem->blocked_threads[i]->num == tcb_arr[cur_t_ind]->num) {
			my_sem->blocked_threads.erase(my_sem->blocked_threads.begin()+i);
			if(my_sem->blocked_threads.size() == 0) {
				my_sem->blocking = false;
			}
			break;
		}
	}
	//When it unblocks, this will run
	my_sem->value -= 1;
	unlock();
	
}
int sem_post(sem_t *sem) {
	if(tcb_arr.size() == 0) {
		init();
	}
	lock();
	Semaphore *my_sem = (Semaphore*)(sem->__align);
	if(my_sem->value == 65536) {
		return EOVERFLOW;
	}
	my_sem->value++;
	for(int i = 0; i < my_sem->blocked_threads.size(); i++) {
		if(my_sem->blocked_threads[i]->status == 3) {
			my_sem->blocked_threads[i]->status = 1;
			break;
		}
	}

	unlock();

	return 0;
}

