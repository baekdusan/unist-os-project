#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h> //여기서 list.h를 include하고 있어서 thread.c에서 list를 사용할 수 있던 것.
#include <stdint.h>
#include "synch.h"

/* States in a thread's life cycle. */
enum thread_status
{
   THREAD_RUNNING, /* Running thread. */
   THREAD_READY,   /* Not running but ready to run. */
   THREAD_BLOCKED, /* Waiting for an event to trigger. */
   THREAD_DYING    /* About to be destroyed. */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t)-1) /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0      /* Lowest priority. */
#define PRI_DEFAULT 31 /* Default priority. */
#define PRI_MAX 63     /* Highest priority. */
#define F (1 << 14)    // mlfq에서 fixed pointer에서 사용.

/* A kernel thread or user process.

   Each thread structure is stored in its own 4 kB page.  The
   thread structure itself sits at the very bottom of the page
   (at offset 0).  The rest of the page is reserved for the
   thread's kernel stack, which grows downward from the top of
   the page (at offset 4 kB).  Here's an illustration:

        4 kB +---------------------------------+
             |          kernel stack           |
             |                |                |
             |                |                |
             |                V                |
             |         grows downward          |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             +---------------------------------+
             |              magic              |
             |                :                |
             |                :                |
             |               name              |
             |              status             |
        0 kB +---------------------------------+

   The upshot of this is twofold:

      1. First, `struct thread' must not be allowed to grow too
         big.  If it does, then there will not be enough room for
         the kernel stack.  Our base `struct thread' is only a
         few bytes in size.  It probably should stay well under 1
         kB.

      2. Second, kernel stacks must not be allowed to grow too
         large.  If a stack overflows, it will corrupt the thread
         state.  Thus, kernel functions should not allocate large
         structures or arrays as non-static local variables.  Use
         dynamic allocation with malloc() or palloc_get_page()
         instead.

   The first symptom of either of these problems will probably be
   an assertion failure in thread_current(), which checks that
   the `magic' member of the running thread's `struct thread' is
   set to THREAD_MAGIC.  Stack overflow will normally change this
   value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
   the run queue (thread.c), or it can be an element in a
   semaphore wait list (synch.c).  It can be used these two ways
   only because they are mutually exclusive: only a thread in the
   ready state is on the run queue, whereas only a thread in the
   blocked state is on a semaphore wait list. */
struct thread // thread structure있는 곳.
{
   /* Owned by thread.c. */
   tid_t tid;                 /* Thread identifier. */
   enum thread_status status; /* Thread state. */
   char name[16];             /* Name (for debugging purposes). */
   uint8_t *stack;            /* Saved stack pointer. */
   int priority;              /* Priority. */
   // 전역 allelem에 들어갈 double linked list의 구성요소.
   struct list_elem allelem; /* List element for all threads list. */

   /* Shared between thread.c and synch.c. */
   // 나중에 lock같은거 얻을 때 기다리는 thread이런거 저장할 때 쓰나봄.
   // 특정 list에 쓰도록 두는 double linked list 요소? thread_yeild 함수에도 쓰임
   // 그런걸 보니까 더더욱이 맞는듯.
   struct list_elem elem; /* List element. */

   /*Alarm Clock 때 추가 */

   int64_t tick_wakeup; // local tick thread가 sleep일 때, 일어나야 하는 tick저장.

   /* Priority 때 추가 */

   int original_priority; // priority donation 되었을 때, release하면 restore에 필요한 것.

   struct lock *wait_lock;  // 한 thread어차피 한 lock밖에 대기 못함. 해당 lock 구조체 주소 저장.
   struct list_elem d_elem; // donation element
   struct list donations;   // multiple donation 문제를 해결하기 위한 list.

   /* mlfq 때 추가 */
   int nice;
   int recent_cpu;

#ifdef USERPROG
   /* Owned by userprog/process.c. */
   uint32_t *pagedir; /* Page directory. */
#endif

   /* Owned by thread.c. */
   unsigned magic; /* Detects stack overflow. */
   // syscall때 구현
   struct file *file_descriptor[64];
   int next_fd;
   int exit_status; //종료상태 가지고 있기.
   // struct file *exec_file; //현재 실행중인 파일.
   // struct list_elem child_elem; //parent가 child가질 때 사용하는 것
   // struct list child_list;  //이 list하나만 있으면 head ptr이런것도 필요 없을듯.
   // struct thread *parent; //부모 쓰레드
   // struct semaphore wait_sema; //자식 종료 대기시 사용.
   // struct semaphore exec_sema; //자식 프로그램 만들어지는거 대기 시 사용.
   // int load_success; //load 성공 여부.
};

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init(void);
void thread_start(void);

void thread_tick(void);
void thread_print_stats(void);

typedef void thread_func(void *aux);
tid_t thread_create(const char *name, int priority, thread_func *, void *);

void thread_block(void);
void thread_unblock(struct thread *);

struct thread *thread_current(void);
tid_t thread_tid(void);
const char *thread_name(void);

void thread_exit(void) NO_RETURN;
void thread_yield(void);
void check_preempt(void);

bool priority_large(const struct list_elem *a, const struct list_elem *b, void *aux);
bool tick_less(const struct list_elem *a, const struct list_elem *b, void *aux); // 이거 구현했음.
void thread_sleep(int64_t ticks);                                                // thread.c에 추가한 함수.
void thread_wakeup(int64_t ticks);                                               // thread.c에 추가한 함수. ticks보다 작거나 같은거 다 ready로 옮김.

/* Performs some operation on thread t, given auxiliary data AUX. */
typedef void thread_action_func(struct thread *t, void *aux);
void thread_foreach(thread_action_func *, void *);

int thread_get_priority(void);
void thread_set_priority(int);

int thread_get_nice(void);
void thread_set_nice(int);
int thread_get_recent_cpu(void);
int thread_get_load_avg(void);

extern int64_t global_wakeup_tick; // thread.c에 추가한 전역변수.인데 이렇게 하면 timer에 연결 되려나..?

int convert_n_to_fixed_point(int n);
int convert_x_to_integer_rounding_zero(int x);
int convert_x_to_integer_rounding_nearest(int x);
int add_x_and_y(int x, int y);
int substract_y_from_x(int x, int y);
int add_x_and_n(int x, int n);
int substract_n_from_x(int x, int n);
int multiply_x_by_y(int x, int y);
int multiply_x_by_n(int x, int n);
int divide_x_by_y(int x, int y);
int divide_x_by_n(int x, int n);
void mlfq_calculate_priority(struct thread *t);
void mlfq_calculate_recent_cpu(struct thread *t);
void mlfq_calculate_load_avg(void);
void mlfq_increment_recent_cpu(void);
void mlfq_recalculate_priority_all_threads(void);
void mlfq_recalculate_recent_cpu_priority_all_threads(void);

#endif /* threads/thread.h */
