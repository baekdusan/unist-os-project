#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
     decrement it.

   - up or "V": increment the value (and wake up one waiting
     thread, if any). */
void
sema_init (struct semaphore *sema, unsigned value) 
{
  ASSERT (sema != NULL);

  sema->value = value;
  list_init (&sema->waiters);
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. */
void
sema_down (struct semaphore *sema) 
{
  enum intr_level old_level;

  ASSERT (sema != NULL);
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  while (sema->value == 0){//sema를 모두 가져가서 대기해야함.//sema_wait list같은 경우는 thread_elem을 넣고 있어서 priority_large만들어둔걸로 사용 가능.
      list_insert_ordered(&sema->waiters, &thread_current ()->elem, priority_large, NULL); // semaphore의 wait list에 current thread의 elem을 large priority로 넣음. prority large thread.h에 있으니까 괜찮겠지.
      thread_block (); //block상태로 만들어주고 schedule()을 호출해줌. 즉, 다시 깨워질 때 까지 wait list에서 대기.
    }
  sema->value--;
  intr_set_level (old_level);
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool
sema_try_down (struct semaphore *sema) 
{
  enum intr_level old_level;
  bool success;

  ASSERT (sema != NULL);

  old_level = intr_disable ();
  if (sema->value > 0) 
    {
      sema->value--;
      success = true; 
    }
  else
    success = false;
  intr_set_level (old_level);

  return success;
}

/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
void
sema_up (struct semaphore *sema) 
{
  enum intr_level old_level;

  ASSERT (sema != NULL);

  old_level = intr_disable ();
  if (!list_empty (&sema->waiters)){
      list_sort(&sema->waiters, priority_large, NULL); //이거는 될 것 같음 왜냐하면 여기 waiter는 thred의 elem이니까.
      thread_unblock (list_entry (list_pop_front (&sema->waiters), struct thread, elem));//현재 해당 sema의 wait list 맨 앞의 elem을 선언한 thread를 깨워줌.
  }
  sema->value++;
  check_preempt(); //block에서 preemp check안해줌. 그래서 일일히 우리가 필요할 때 preempt해줘야함. 전에 이렇게 했다가 오류남.
  intr_set_level (old_level);
}

static void sema_test_helper (void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void
sema_self_test (void) 
{
  struct semaphore sema[2];
  int i;

  printf ("Testing semaphores...");
  sema_init (&sema[0], 0);
  sema_init (&sema[1], 0);
  thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
  for (i = 0; i < 10; i++) 
    {
      sema_up (&sema[0]);
      sema_down (&sema[1]);
    }
  printf ("done.\n");
}

/* Thread function used by sema_self_test(). */
static void
sema_test_helper (void *sema_) 
{
  struct semaphore *sema = sema_;
  int i;

  for (i = 0; i < 10; i++) 
    {
      sema_down (&sema[0]);
      sema_up (&sema[1]);
    }
}

/* Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void
lock_init (struct lock *lock)
{
  ASSERT (lock != NULL);

  lock->holder = NULL;
  sema_init (&lock->semaphore, 1); //lock은 mutex로 구현되어있음. 그래서 lock_acquire이런것도 다 sema_down up이랑 연관됨.
}

void priority_donation(struct thread *cur) {
    struct thread *holder;
    while(cur->wait_lock != NULL){ //cur이 대기중인 ㅣock이 존재하면.
        holder = cur->wait_lock->holder;
        if (cur->priority > holder->priority){
            holder->priority = cur->priority; // 우선순위 갱신
        }
        else{
            break;
        }
        cur = holder; // 다음 락으로 이동.
    }
}

/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */ //음... 알아서 interrupt를 처리한다는 얘기인가??
void
lock_acquire (struct lock *lock)
{
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (!lock_held_by_current_thread (lock));
  if(!thread_mlfqs){ //mlfq mode일 때는 donation을 위한 것들 필요 X.
      if(lock -> holder != NULL){
          struct thread *t = thread_current();
          t->wait_lock = lock; //현재 thread가 대기중인 lock을 설정.
          list_insert_ordered(&lock->holder->donations, &t->d_elem, priority_large, NULL);
          priority_donation(t);
      }
  }

//  if(lock_try_acquire(lock)){ //lock을 얻을 수 있는지 check, 그리고 얻을 수 있으면 얻어버림.
//      return;
//  }
//  else{ //lock을 얻을 수 없으면 현재 holder와 priority 비교 후 priority를 줌.
//      struct thread *t = thread_current(); //현재 thread를 가져옴.
//      t->wait_lock = lock; //현재 thread가 대기중인 lock을 설정.
//      list_push_back (&lock->holder->donations, &t->d_elem); //multi donation해결을 위해 우선 holder에게 donate할 수 있는 thread들을 보관 //둘 다 주소로 줘야함 &사용.
//      //이후 unlock시에 다시 donation list를 priority를 다시 설정해야하는데, list돌면서 푸는 lock을 가지고 있는 요소는 제거하고 다시 돌며 highest or origin중에 최고로 설정하면 될듯.
//      priority_donation(t); //nested 해결을 위해 계쏙 돌아야함.
//  }
  /*기존 코드 */
  sema_down (&lock->semaphore); //mutex down.
  lock->holder = thread_current ();
  thread_current()-> wait_lock = NULL; //sema down 밑으로 내려오면 풀린거니까.
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool
lock_try_acquire (struct lock *lock)
{
  bool success;

  ASSERT (lock != NULL);
  ASSERT (!lock_held_by_current_thread (lock));

  success = sema_try_down (&lock->semaphore);
  if (success)
    lock->holder = thread_current ();
  return success;
}

void reassign_priority(struct thread *cur){ //내가 볼 때, 이걸 호출하는 thread와 연관된거 싹 reassign해줘야 할 것 같은데..
    cur -> priority = cur -> original_priority;
    int highest = cur -> priority;
    if(list_empty(&cur->donations)){
        return;
    }
    else{
        struct list_elem *e;
        for(e = list_begin(&cur->donations); e != list_end(&cur->donations); e = list_next(e)){
            struct thread *t = list_entry(e, struct thread, d_elem);
            if(t->priority > highest){
                highest = t->priority;
            }
        }
        cur->priority = highest;
        priority_donation(cur);
    }
}

/* Releases LOCK, which must be owned by the current thread.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void
lock_release (struct lock *lock) 
{
  ASSERT (lock != NULL);
  ASSERT (lock_held_by_current_thread (lock));

  struct thread *cur = thread_current();
  struct list_elem *e = list_begin(&cur->donations);
  struct list_elem *next;
  if(!thread_mlfqs){ //mlfq일 때는 donation하는 것들 필요 X.
      //이렇게 해서 donation list의 현재 해제하려는 lock의 d_elem을 모두 제거.
      if(!list_empty(&cur->donations)){
          while(e != list_end(&cur->donations)){
              next = list_next(e);
              struct thread *t = list_entry(e, struct thread, d_elem);
              if(t->wait_lock == lock){
                  list_remove(e);
              }
              e = next;
          }
      }
      reassign_priority(cur); //priority를 donation list에서 다시 돌면서 재 assign 해야함.
  }
  lock->holder = NULL;
  sema_up (&lock->semaphore);
}

/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool
lock_held_by_current_thread (const struct lock *lock) 
{
  ASSERT (lock != NULL);

  return lock->holder == thread_current ();
}

/* One semaphore in a list. */
struct semaphore_elem 
  {
    struct list_elem elem;              /* List element. */ //conditional variable의 waitlist에 들어가는 elem
    struct semaphore semaphore;         /* This semaphore. */
  };

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void
cond_init (struct condition *cond)
{
  ASSERT (cond != NULL);

  list_init (&cond->waiters);
}

/* Atomically releases LOCK and waits for COND to be signaled by // lock을 들고 wait을 해야함.
   some other piece of code.  After COND is signaled, LOCK is // signal 받으면 lock을 얻고 다시 실행된다.
   reacquired before returning.  LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with //이게 무슨 소리일까? 함수가 실행될 때, 인터럽트가 비활성화 되어 있어도, 실제로 대기 상태로 들어갈떄는 인터럽트를 재활성화해서 인터럽트가 발생할 수 있도록 한다고 함.
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
bool cond_priority_large(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED){
    struct semaphore_elem *sema_a = list_entry(a, struct semaphore_elem, elem); //semaphore_elem struct를 가져옴.
    struct semaphore_elem *sema_b = list_entry(b, struct semaphore_elem, elem); //semaphore_elem struct를 가져옴.

    struct list_elem *t_elem_a = list_begin(&sema_a->semaphore.waiters); //semaphore_elem내의 slmaphore에서 waiters에서 첫번쨰에 있는 것. 각 thread가 있는 것이니까 해당 thread의 elem이 들어있음.
    struct list_elem *t_elem_b = list_begin(&sema_b->semaphore.waiters);

    struct thread *t_a = list_entry(t_elem_a, struct thread, elem); // elem을 이용해서 thread가져옴.
    struct thread *t_b = list_entry(t_elem_b, struct thread, elem);

    return t_a->priority > t_b->priority;
}

void
cond_wait (struct condition *cond, struct lock *lock) //lock을 가지 실행됨.
{
  struct semaphore_elem waiter; //음... 왜 sema를 쓰지? 어쨌든 list_elem과 semaphore를 선언함. 위에 선언되어있음 구조체
  //conditional variable의 wait list에 대기할 elem은 semaphore_elem이다. 이 구조체에는 elem과 semaphore가 있다.
  //그래서 signal로 up 시켜줄 때 까지 자고 있을 수 있는 것임.
  //이 semaphore_elem으로 인한 semaphore는 이걸 호출한 thread단 하나만 들어있는 semaphore임. 그러므로 down을 통해서
  //현재 thread의 elem을 semaphore의 waiters에 담으니까 비교할 때, semaphore_elem을 먼저 찾고
  //그리고 그 안에 semaphore의 waiters에 있는 elem을 이용해서 thread를 찾으면 priority 비교가능.

  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));
  
  sema_init (&waiter.semaphore, 0); //semaphore를 0으로 초기화 하면 scheduling과 같음.
  list_insert_ordered(&cond->waiters, &waiter.elem, cond_priority_large, NULL); //conditional variable의 wait list에 현재 thread의 elem을 넣어줌. priority large로 넣어줌.
  lock_release (lock); //thread가 cond_wait시에 호출했던 lock을 자동으로 푼다.
  sema_down (&waiter.semaphore); //그리고 새로 선언한 semaphore를 down시킨다. current thread를 넣음. signal 받을 때 까지 대기.
  lock_acquire (lock); //다시 꺠어날 때 lock을 얻은 상태.
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) 
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));

  if (!list_empty (&cond->waiters)){//conditional variable에 waiter가 존재하면
      list_sort(&cond->waiters, cond_priority_large, NULL);
      //conditional varialbe 맨 앞 wait list에서 pop해주고 해당 sema_elem을 들고있는 구조체 주소 반환. 그리고 해당 sema를 up시켜줌.
      sema_up (&list_entry (list_pop_front (&cond->waiters), struct semaphore_elem, elem)->semaphore);//cond waiter list의 맨 앞에서 꺼내서 그 elem을 들고있던 thread의 sema를 up시킴.
  }

}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_broadcast (struct condition *cond, struct lock *lock) 
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);

  while (!list_empty (&cond->waiters))
    cond_signal (cond, lock);
}
