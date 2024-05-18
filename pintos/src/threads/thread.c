#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list; // static 변수는 프로그램의 시작부터 종료까지 메모리에 존재함. 이게 선언된 소스파일 내에서만 접근 가능. 여기서 선언을 해주고 있음.

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

static struct list sleep_list; // busy waiting을 없애기 위한 sleep list 추가.

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame
{
  void *eip;             /* Return address. */
  thread_func *function; /* Function to call. */
  void *aux;             /* Auxiliary data for function. */
};

// sleep list안의 최소 tick을 유지. timer.c에서도 접근 가능해야해서 global로 선언.
int64_t global_wakeup_tick = INT64_MAX; // thread.c/h에서 timer.c/h를 include해주지 않아서 여기에 함.

/* Statistics. */
static long long idle_ticks;   /* # of timer ticks spent idle. */
static long long kernel_ticks; /* # of timer ticks in kernel threads. */
static long long user_ticks;   /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4          /* # of timer ticks to give each thread. */
static unsigned thread_ticks; /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs; // 이걸로 mlfq 키는 것임. 그래서 사실 if thread_mlfqs == false이렇게 사용하면 되겠네.
int load_avg;      // static으로 선언했으니까 다른 파일에서 쓰려면 getter써야하는데 어차피 getter있으니까. setter만들어주면 될듯.
// #define PRI_MAX 63 //이런 constant들 thread.h에 있어서 그냥 사용하면 됨.

static void kernel_thread(thread_func *, void *aux);

static void idle(void *aux UNUSED);
static struct thread *running_thread(void);
static struct thread *next_thread_to_run(void);
static void init_thread(struct thread *, const char *name, int priority);
static bool is_thread(struct thread *) UNUSED;
static void *alloc_frame(struct thread *, size_t size);
static void schedule(void);
void thread_schedule_tail(struct thread *prev);
static tid_t allocate_tid(void);

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void thread_init(void) // main에서 단 한번 실행하는 부분임. 그래서 list같은거 init을 여기서 해주는 것임. init.c의 main에서 호출함.
{
  ASSERT(intr_get_level() == INTR_OFF);

  lock_init(&tid_lock);
  list_init(&ready_list);
  list_init(&all_list);
  list_init(&sleep_list); // sleep list 추가했음.

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread();
  init_thread(initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void thread_start(void)
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init(&idle_started, 0);
  thread_create("idle", PRI_MIN, idle, &idle_started);
  load_avg = 0; // load_avg 초기화.

  /* Start preemptive thread scheduling. */
  intr_enable();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down(&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void thread_tick(void)
{
  struct thread *t = thread_current();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return();
}

/* Prints thread statistics. */
void thread_print_stats(void)
{
  printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
         idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t thread_create(const char *name, int priority,
                    thread_func *function, void *aux) // thread create 시 초기화 되는 것들. 초기 priority도 이때 받아서 설정되네.
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT(function != NULL);

  /* Allocate thread. */
  t = palloc_get_page(PAL_ZERO); //allocating one page
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread(t, name, priority); // 여기서 init_thread실행함. thread structure를 초기화하는 곳.
  tid = t->tid = allocate_tid(); // tid할당.

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame(t, sizeof *kf); //stack할당.
  kf->eip = NULL;
  kf->function = function; // 실행할 function address
  kf->aux = aux; //실행할 function의 parameter

  /* Stack frame for switch_entry(). */
  ef = alloc_frame(t, sizeof *ef);
  ef->eip = (void (*)(void))kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame(t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* Add to run queue. */
  thread_unblock(t);
  if (t->priority > thread_current()->priority)
  { // 새롭게 추가된 thread가 현재 thread보다 priority 높으면 바로 yield.
    thread_yield();
  }

  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void thread_block(void)
{
  ASSERT(!intr_context());
  ASSERT(intr_get_level() == INTR_OFF);

  thread_current()->status = THREAD_BLOCKED;
  schedule();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
bool priority_large(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
  struct thread *thread_a = list_entry(a, struct thread, elem);
  struct thread *thread_b = list_entry(b, struct thread, elem);
  return thread_a->priority > thread_b->priority; // a의 priority가 높을 때 true less function이 true면 멈추고 그 자리에 넣음.
}

void thread_unblock(struct thread *t) // block상태에 있던 thread state 변경 후 ready queue에 넣어주는 코드.
{
  enum intr_level old_level;

  ASSERT(is_thread(t));

  old_level = intr_disable();
  ASSERT(t->status == THREAD_BLOCKED);
  list_insert_ordered(&ready_list, &t->elem, priority_large, NULL);
  t->status = THREAD_READY; // 이렇게 해보려고 했는데 test에서 timeout남. slide대로 thread_create에만 넣자.
  intr_set_level(old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name(void)
{
  return thread_current()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current(void)
{
  struct thread *t = running_thread();

  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT(is_thread(t));
  ASSERT(t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t thread_tid(void)
{
  return thread_current()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void thread_exit(void)
{
  ASSERT(!intr_context());

#ifdef USERPROG
  process_exit();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable();
  list_remove(&thread_current()->allelem);
  thread_current()->status = THREAD_DYING;
  schedule();
  NOT_REACHED();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void thread_yield(void) // 여기 thread_yield
{
  struct thread *cur = thread_current(); // 현재 thread structure pointer.
  enum intr_level old_level;

  ASSERT(!intr_context());

  old_level = intr_disable();                                         // 아마 현재 interrupt level을 반환하는 것 같음. //disable the interrupt and return previous interrupt state
  list_insert_ordered(&ready_list, &cur->elem, priority_large, NULL); // 이렇게 안하면 idle도 재실행이 안될 것 같아서 위랑 둘 중에 뭐가 맞는지 모르겠음.
  cur->status = THREAD_READY;
  schedule();                // context switch
  intr_set_level(old_level); // interupt state 되돌리기 //set a state of interrupt to the state passed to parameter and return previous interrupt state
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void thread_foreach(thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT(intr_get_level() == INTR_OFF);

  for (e = list_begin(&all_list); e != list_end(&all_list);
       e = list_next(e))
  {
    struct thread *t = list_entry(e, struct thread, allelem);
    func(t, aux);
  }
}

void check_preempt(void)
{ // 현재 thread의 priority와 ready list맨앞과의 priority를 비교해서 ready가 높으면 yield.
  if (!list_empty(&ready_list))
  {
    struct thread *ready_list_first_thread = list_entry(list_begin(&ready_list), struct thread, elem);
    if (thread_current()->priority < ready_list_first_thread->priority)
    {
      if (thread_current() != idle_thread)
      {
        thread_yield();
      }
    }
  }
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void thread_set_priority(int new_priority)
{
  if (thread_mlfqs)
  { // initially false라고 되어있음.
    return;
  }
  thread_current()->original_priority = new_priority;
  // 이렇게 priority를 바꾸고 나면 다시 ready list를 정렬해줘야함.

  reassign_priority(thread_current()); // priority재설정해줌 지금 새로 assign된걸로 priority 변경하고 donationlist에서도 다시 찾고.
  enum intr_level old_level = intr_disable();
  list_sort(&ready_list, priority_large, NULL); // 일단 slide에 따르면 이것만 하는데 추가로 schduling을 해줘야 하는지는 의문임.
  intr_set_level(old_level);
  // priority_donation(thread_current()); //reassign에서 이미 이것도 호출해서 필요없음.
  // 아 current thread의 priority가 낮아질 수도 있구나. 그러니까 이걸 check해줘야함.
  check_preempt(); // 나중에 더 써야하는 것이라 함수로 만듬.
}

/* Returns the current thread's priority. */
int thread_get_priority(void)
{
  return thread_current()->priority;
}

/* Sets the current thread's nice value to NICE. */
void thread_set_nice(int nice)
{
  enum intr_level old_level;
  old_level = intr_disable();
  thread_current()->nice = nice;
  intr_set_level(old_level);
}

/* Returns the current thread's nice value. */
int thread_get_nice(void)
{
  enum intr_level old_level;
  old_level = intr_disable();
  int result = thread_current()->nice; // 이렇게 해야 int로 바꿔줌.
  intr_set_level(old_level);
  return result;
}

/* Returns 100 times the system load average. */
int thread_get_load_avg(void) // load_avg값은 float이다.
{
  enum intr_level old_level;
  old_level = intr_disable();
  int result = convert_x_to_integer_rounding_zero(multiply_x_by_n(load_avg, 100)); // 이렇게 해야 int로 바꿔줌.
  intr_set_level(old_level);
  return result;
}

/* Returns 100 times the current thread's recent_cpu value. */
int thread_get_recent_cpu(void) // recent cpu도 float이다.
{
  enum intr_level old_level;
  old_level = intr_disable();
  int result = convert_x_to_integer_rounding_zero(multiply_x_by_n(thread_current()->recent_cpu, 100)); // 이렇게 해야 int로 바꿔줌.
  intr_set_level(old_level);
  return result;
}

void mlfq_calculate_priority(struct thread *t)
{ // thread 받아서 해당 thread priority 재계산.
  if (t == idle_thread)
  {
    return;
  }
  int f1 = convert_n_to_fixed_point(PRI_MAX); // 왜 이게 차이나지??
  int f2 = divide_x_by_n(t->recent_cpu, 4);
  int f3 = convert_n_to_fixed_point(t->nice * 2);
  int priority = f1 - f2 - f3;
  priority = convert_x_to_integer_rounding_zero(priority); // 이거 int로 바꿔주는 것임.
  if (priority > PRI_MAX)
  {
    priority = PRI_MAX;
  }
  else if (priority < PRI_MIN)
  {
    priority = PRI_MIN;
  }
  t->priority = priority;
}

void mlfq_calculate_recent_cpu(struct thread *t)
{ // recent cpu계산. every second마다 decay니까 factor recent cpu 계싼시에 계싼하면 됨.
  if (t == idle_thread)
  {
    return;
  }
  int f1 = multiply_x_by_n(load_avg, 2);
  int f2 = add_x_and_n(f1, 1);
  int decay = divide_x_by_y(f1, f2);
  t->recent_cpu = add_x_and_n(multiply_x_by_y(decay, t->recent_cpu), t->nice);
}

void mlfq_calculate_load_avg(void)
{ // 현재 load_avg갱신.
  enum intr_level old_level;
  old_level = intr_disable();
  int ready_threads = 0; // ready threads의 수는 ready queue에 있는 thread의 수 + 실행중인 thread(except idle)
  for (struct list_elem *e = list_begin(&ready_list); e != list_end(&ready_list); e = list_next(e))
  {
    struct thread *t = list_entry(e, struct thread, allelem);
    if (t != idle_thread)
    {
      ready_threads++;
    }
  }
  if (thread_current() != idle_thread)
  {
    ready_threads++;
  }
  int f0 = convert_n_to_fixed_point(59);
  int f01 = convert_n_to_fixed_point(60);
  int f1 = divide_x_by_y(f0, f01);
  int f2 = divide_x_by_n(convert_n_to_fixed_point(1), 60);
  int f3 = multiply_x_by_y(f1, load_avg);
  f3 = multiply_x_by_n(divide_x_by_y(f3, convert_n_to_fixed_point(1)), 1);
  int f4 = multiply_x_by_y(f2, convert_n_to_fixed_point(ready_threads));
  load_avg = add_x_and_y(f3, f4);
  intr_set_level(old_level);
}

void mlfq_increment_recent_cpu(void)
{ // 매 tick마다 1씩 늘려야함.
  if (thread_current() != idle_thread)
  {
    thread_current()->recent_cpu = add_x_and_n(thread_current()->recent_cpu, 1); // float이니까.
  }
}

void mlfq_recalculate_priority_all_threads(void)
{ // 매 4 tick마다 모든 thread의 priority재계산해야함.
  struct list_elem *e;
  for (e = list_begin(&all_list); e != list_end(&all_list); e = list_next(e))
  {
    struct thread *t = list_entry(e, struct thread, allelem);
    mlfq_calculate_priority(t);
  }
}

void mlfq_recalculate_recent_cpu_priority_all_threads(void)
{ // 매 1sec 마다 모든 thread의 recent_cpu재계산해야함.
  enum intr_level old_level;
  old_level = intr_disable();
  struct list_elem *e;
  mlfq_calculate_load_avg(); // 전부 계산 전에 load_avg갱신.
  for (e = list_begin(&all_list); e != list_end(&all_list); e = list_next(e))
  {
    struct thread *t = list_entry(e, struct thread, allelem);
    mlfq_calculate_recent_cpu(t);
    mlfq_calculate_priority(t);
  }
  intr_set_level(old_level);
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle(void *idle_started_ UNUSED)
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current();
  sema_up(idle_started);

  for (;;)
  {
    /* Let someone else run. */
    intr_disable();
    thread_block();

    /* Re-enable interrupts and wait for the next one.

       The `sti' instruction disables interrupts until the
       completion of the next instruction, so these two
       instructions are executed atomically.  This atomicity is
       important; otherwise, an interrupt could be handled
       between re-enabling interrupts and waiting for the next
       one to occur, wasting as much as one clock tick worth of
       time.

       See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
       7.11.1 "HLT Instruction". */
    asm volatile("sti; hlt" : : : "memory");
  }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread(thread_func *function, void *aux)
{
  ASSERT(function != NULL);

  intr_enable(); /* The scheduler runs with interrupts off. */
  function(aux); /* Execute the thread function. */
  thread_exit(); /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread(void)
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm("mov %%esp, %0" : "=g"(esp));
  return pg_round_down(esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread(struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread(struct thread *t, const char *name, int priority)
{
  enum intr_level old_level;

  ASSERT(t != NULL);
  ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT(name != NULL);

  memset(t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy(t->name, name, sizeof t->name);
  t->stack = (uint8_t *)t + PGSIZE;
  t->priority = priority;
  t->original_priority = priority; // original_priority 초기화
  t->magic = THREAD_MAGIC;
  list_init(&t->donations); // donation list 초기화

  /* 아래는 mlfq*/
  t->nice = 0;
  t->recent_cpu = 0;

  old_level = intr_disable();
  list_push_back(&all_list, &t->allelem);
  intr_set_level(old_level);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame(struct thread *t, size_t size)
{
  /* Stack data is always allocated in word-size units. */
  ASSERT(is_thread(t));
  ASSERT(size % sizeof(uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run(void) // ready list의 맨 앞에 있는 thread를 다음 run으로 잡음.
{
  if (list_empty(&ready_list))
    return idle_thread;
  else
    return list_entry(list_pop_front(&ready_list), struct thread, elem);
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void thread_schedule_tail(struct thread *prev) // 이거로 schedule()실행 후 마무리로 status 바꿔주나봄.
{
  struct thread *cur = running_thread();

  ASSERT(intr_get_level() == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread)
  {
    ASSERT(prev != cur);
    palloc_free_page(prev);
  }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule(void)
{
  struct thread *cur = running_thread();
  struct thread *next = next_thread_to_run(); // 현재 ready list의 맨앞에 위치한 것을 잡음.
  struct thread *prev = NULL;

  ASSERT(intr_get_level() == INTR_OFF);
  ASSERT(cur->status != THREAD_RUNNING);
  ASSERT(is_thread(next));

  if (cur != next)
    prev = switch_threads(cur, next); // switch.S에 구현되어있음. register value 4개 교체해주는 작업.
  thread_schedule_tail(prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid(void)
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire(&tid_lock);
  tid = next_tid++;
  lock_release(&tid_lock);

  return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof(struct thread, stack);

bool tick_less(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{                                                               /// 이거 list에 선언했어서 오류났었음. threadstruct 다 되기 전에 선언했다고. list파일이 thread보다 먼저 compile되나봄.
  struct thread *thread_a = list_entry(a, struct thread, elem); // list_entry는 앞과 같이 사용하면 elem의 thread struct를 반환해줌.
  struct thread *thread_b = list_entry(b, struct thread, elem);
  return thread_a->tick_wakeup < thread_b->tick_wakeup; // a가 작을 때 true를 반환해주면 오름차순으로 정렬됨.
}

// project1 thread sleep 함수 추가. cur안넣어줘서 여기서 cur 받으면 됨. 해당 thread가 실행할 함수니까.
void thread_sleep(int64_t ticks)
{
  enum intr_level old_level;
  struct thread *t = thread_current(); // running_thread()를 실행한 것이 맞지만 sanity check이 추가된 버전임.
  if (t == idle_thread)
  {
    return; // 일단 idle thread일 떄 어떻게 처리하라는 것이 없음. 그리고 idle이 이걸 호출하지도 않을듯.
  }
  else
  {
    old_level = intr_disable();
    t->status = THREAD_BLOCKED;
    t->tick_wakeup = ticks;
    if (global_wakeup_tick > ticks)
    { // 이건 global인데 lock안해줘도 되나?? 근데 이미 disable intrrupt상태.
      global_wakeup_tick = ticks;
    }
    //        list_push_back(&sleep_list, &t->elem); // list에 넣을 때 이렇게 넣음. 정렬은 언제하지??
    list_insert_ordered(&sleep_list, &t->elem, tick_less, NULL); // tick이 작은 순서로 정렬됨.
    schedule();                                                  // context switch해줌.
    intr_set_level(old_level);                                   // ready_list부분에서도 이렇게 list 수정시 intrdisable해줌. 이렇게하면 이 thread가 다시 깨어날 때, intr를 다시 푸는건데 괜찮나..?
                                                                 // 근데 위에 구현된 thread_yeild()에서도 interrupt enable의 위치가 여기가 맞음.
  }
}
// list.c에 tick_less함수를 구현해주었음.

/// list ordered를 사용하려면 아래와 같이 어떤 것을 낮은 oreder로 할지를 설정해줘야함.
///* Compares the value of two list elements A and B, given
//   auxiliary data AUX.  Returns true if A is less than B, or
//   false if A is greater than or equal to B. */
// typedef bool list_less_func (const struct list_elem *a,
//                             const struct list_elem *b,
//                             void *aux);
// void list_insert_ordered (struct list *, struct list_elem *,
//                          list_less_func *, void *aux);

void thread_wakeup(int64_t ticks)
{ // 받은 ticks는 현재 system의 tick.
  if (list_empty(&sleep_list))
  {
    global_wakeup_tick = INT64_MAX; // 최대로 설정해야 나중에 list에 추가되는 thread의 tick으로 global이 변경됨.
    return;
  }
  struct list_elem *e;
  struct thread *t;
  enum intr_level old_level;
  old_level = intr_disable();
  e = list_begin(&sleep_list);
  while (e != list_end(&sleep_list))
  {                                         // list_end의 는 tail임 그래서 이렇게 하면 전체 다 돌음.
    t = list_entry(e, struct thread, elem); // 현재 elem의 thread struct. elem인지 list_elem인지 헷갈림.
    if (t->tick_wakeup <= ticks)
    {                     // 이미 깨어야 할 시간이 되었거나 지난 상태.
      e = list_remove(e); // 다음거 받기.
      thread_unblock(t);  // thread를 ready list에 넣음. ready state로도 변경됨.
    }
    else
    {
      global_wakeup_tick = t->tick_wakeup; // global tick을 다음에 깨워야 하는 thread의 tick으로 설정.
      break;
    }
  }
  intr_set_level(old_level);
}

/* mlfq를 위한 17.14 format functions들 */
// kernel이 정수만을 reg애 저장을 할 수 있기에 integer값을 float으로 매핑할 수 있는 것을 만들었음.
// 즉, int하나의 값이 32bit이니까 이거를 1/17/14로 나누어서 맨 앞은 sign, 17은 int, 14는 소숫점 아래를 나타냄.
// 그래서 이런 규칙을 만들어놓으면 int로 저장해도 float을 얻을 수 있음.
// #define F (1 << 14) //2^14즉, 이게 17.14 format에서는 1.0임. thread.h에 정의해놓음.
int convert_n_to_fixed_point(int n)
{
  return n * F; // 그런 1이 n개.
}

int convert_x_to_integer_rounding_zero(int x)
{
  return x / F; // 이렇게 하면 정수만 반환되니까. 소숫점은 다 버린 것.
}

int convert_x_to_integer_rounding_nearest(int x)
{
  if (x >= 0)
  {
    return (x + F / 2) / F; // 이렇게 하면 반올림이 되는 것임.
  }
  else
  {
    return (x - F / 2) / F; // 음수일 때는 반올림이 되는 것임.
  }
}

int add_x_and_y(int x, int y)
{
  return x + y;
}

int substract_y_from_x(int x, int y)
{
  return x - y;
}

int add_x_and_n(int x, int n)
{
  return x + n * F;
}

int substract_n_from_x(int x, int n)
{
  return x - n * F;
}

int multiply_x_by_y(int x, int y)
{ // F *F하는거니까 F로 나눠야지
  return ((int64_t)x) * y / F;
}

int multiply_x_by_n(int x, int n)
{ // F가 그냥 몇 배 되는거니까 맞는거고.
  return x * n;
}

int divide_x_by_y(int x, int y)
{
  if (y == 0)
  {
    return 0;
  }
  return ((int64_t)x) * F / y;
}

int divide_x_by_n(int x, int n)
{
  if (n == 0)
  {
    return 0;
  }
  return x / n;
}
