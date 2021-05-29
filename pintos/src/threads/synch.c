/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
*/

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

/* The kernel page containing the lock-priority records of all threads.
   There should be as many records as there are locks. */
static void *lock_priority_page;
bool priority_donation_enabled = false;

/* Initalizes the kernel-wide lock_priority_page. This should only be called
   after the page allocator is initialized. */
void
synch_init (void) {
  lock_priority_page = palloc_get_page(PAL_ZERO);
  priority_donation_enabled = true;
}

/* Initialize and returns a new lock_priority structure. Null is returned if no
 * more space is available.
 *
 * NOTE: since this function operate on shared data structure, it must be called
 * with interrupt turned off.
 */
static struct lock_priority *get_new_lock_priority(void) {
  int limit = PGSIZE / sizeof(struct lock_priority);
  struct lock_priority *page = (struct lock_priority *) lock_priority_page;

  /*
   * iterate through the kernel-wide lock_priority_page, and return the first
   * available record.
   */
  for (int index = 0; index < limit; index++) {
    if (!page[index].in_use) {
      page[index].in_use = true;
      return &page[index];
    }
  }

  return NULL;
}

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
  while (sema->value == 0)
    {
      list_push_back (&sema->waiters, &thread_current ()->elem);
      thread_block ();
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

  /*
   * The semaphore value must be incremented before waking up a waiter thread,
   * otherwise the awakened thread still won't be able to down the value and
   * will simply be put on the waitlist once again.
   */
  sema->value++;

  if (!list_empty (&sema->waiters)) {
    struct list_elem *thread_elem = list_max(&sema->waiters, default_list_less_func, thread_priority_convert_func);
    list_remove(thread_elem);
    struct thread *t = list_entry(thread_elem, struct thread, elem);
    thread_unblock(t);
    
    // if the awaken thread has a higher priority, preempt the current thread
    if (priority_donation_enabled && t->priority > thread_get_priority())
      thread_yield();
  }

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

/*
 * Find and return the lock_priority corresponding to LOCK in the PRIORITY_LIST.
 * Return NULL if not found.
 */
static struct lock_priority *
find_lock_priority(struct list *priority_list, struct lock *lock) {
  struct list_elem *current_elem = list_begin(priority_list);
  struct list_elem *end_elem = list_end(priority_list);
  struct lock_priority *lock_priority = NULL;
  
  // find the lock_priority corresponding to LOCK; make sure that there is one.
  for (; current_elem != end_elem; current_elem = list_next(current_elem)) {
    lock_priority = list_entry(current_elem, struct lock_priority, elem);
    if (lock_priority->lock == (void *) lock)
      break;
  }

  if (current_elem == end_elem)
    return NULL;
  else
    return lock_priority;
}

/*
 * Donate the PRIORITY to the holder of LOCK, updating the holder thread's effective
 * priority, as well as the priority of the corresponding lock_priority. Returns
 * true if the priority was donated, false otherwise.
 */
static bool
donate_priority(struct lock *lock, int priority) {
  struct thread *holder = lock->holder;
  struct lock_priority *holder_lock_priority = find_lock_priority(&holder->lock_priority_list, lock);

  /*
   * Evaluate the holder's effective priority and the lock_priority separately.
   * If either variable is lower than the give priority, use the given priority
   * as the new value for that variable.
   * 
   * Only claim that the priority is donated if the holder's effective priority
   * is increased. If the update only applied to the priority of lock_priority,
   * then the holder has a higher effective priority, and recursive donation
   * won't be necessary.
   */
  bool donated = false;
  if (priority > holder->priority) {
    holder->priority = priority;
    donated = true;
  }
  if (priority > holder_lock_priority->priority)
    holder_lock_priority->priority = priority;

  return donated;
}

/*
 * Donate the PRIORITY to the owner of LOCK, and also recursively to the owner
 * of any other lock that this owner is waiting for.
 */
static void
donate_priority_recursively(struct lock *lock, int priority) {
  ASSERT (lock != NULL);
  ASSERT (lock->holder != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);

  /*
   * First, try to donate PRIRORITY to the holder of LOCK. If donated, check if
   * the holder is waiting for another lock, and try to donate recursively.
   * 
   * If the donation wasn't necessary, then the holder of LOCK has a higher
   * effective priority, and it would have already donated that priority to
   * any lock it's waiting on, so there's no need to donate PRIORITY recursively.
   */
  if (donate_priority(lock, priority)) {
    struct thread *holder = lock->holder;
    struct list_elem *top_elem = list_begin(&holder->lock_priority_list);
    ASSERT (top_elem != list_end(&holder->lock_priority_list));
    struct lock_priority *top_lock_priority = list_entry(top_elem, struct lock_priority, elem);

    /*
     * It is possible that holder of LOCK was waiting for another lock that has
     * just been released, in which case top_lock_priority->waiting will be true,
     * but top_lock_priority->lock->holder will be NULL. Had the other lock have
     * an owner, our donor can donate to that owner recursively, hopefully
     * speeding up the execution of that owner's critical section, so that it
     * could release the lock that the holder of LOCK is waiting for. However,
     * if the other lock doesn't have an owner, it is impossible for our donor
     * to make it quicker for the holder of LOCK to acquire the other lock. And
     * therefore, there is no need for recursive donation if the other lock has
     * already been released.
     */
    if (top_lock_priority->waiting && ((struct lock *) top_lock_priority->lock)->holder != NULL)
      donate_priority_recursively((struct lock *) top_lock_priority->lock, priority);
  }

  return;
}

// Converts a (struct lock_priority).elem to the lock_priority's priority.
static int lock_priority_convert_func(const struct list_elem *e) {
  return list_entry(e, struct lock_priority, elem)->priority;
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
  sema_init (&lock->semaphore, 1);
}

/* Acquires LOCK, sleeping until it becomes available if necessary. The lock
   must not already be held by the current thread.

   This function may sleep, so it must not be called within an interrupt handler.
   This function may be called with interrupts disabled, but it will re-attempt
   to disable interrupt. Interrupts will be enabled again when the thread goes
   to sleep. */
void
lock_acquire (struct lock *lock)
{
  /*
   * Disabling the interrupt, because the assertions and the access to other
   * thread's kernel data must be performed atomically.
   */
  enum intr_level old_level = intr_disable();
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (!lock_held_by_current_thread (lock));
  
  struct thread *t = thread_current();
  struct lock_priority *new_lock_priority = NULL;


  if (priority_donation_enabled) {
    /*
     * Add a new lock_priority to the executing thread's priority list. This
     * must be done before the executing thread successfully acquires the lock,
     * because if a second thread needs a lock A held by the executing thread,
     * but the executing thread is waiting for a lock B held by a third thread,
     * then the second thread needs to recursively donate its priority to the
     * third thread as well. In order for the second thread to know which lock
     * the executing thread is waiting for, the executing thread must push that
     * lock's lock_priority into its priority list.
     */
    new_lock_priority = get_new_lock_priority();
    ASSERT(new_lock_priority != NULL);

    new_lock_priority->waiting = true;
    new_lock_priority->lock = (void *) lock;
    new_lock_priority->priority = thread_get_priority();
    list_push_front(&t->lock_priority_list, &new_lock_priority->elem);

    /*
     * if the lock already has an owner, the executing thread might need to donate
     * its priority to the lock owner recursively.
     */
    if (lock->holder != NULL)
      donate_priority_recursively(lock, thread_get_priority());
  }

  // otherwise, call sema_down() to wait and acquire the lock.
  sema_down (&lock->semaphore);

  /*
   * After acquiring the lock, we need to double check that the interrupt is
   * disabled, then update the lock->holder to the current thread;
   */
  ASSERT (intr_get_level() == INTR_OFF);
  lock->holder = t;
  // mark the lock_priority as owned
  if (priority_donation_enabled)
    new_lock_priority->waiting = false;

  intr_set_level(old_level);
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

/* Releases LOCK, which must be owned by the current thread.

   An interrupt handler cannot acquire a lock, so it does not make sense to try
   to release a lock within an interrupt handler. */
void
lock_release (struct lock *lock)
{
  /*
   * Disabling the interrupt, because the assertions and the access to the lock
   * owner thread's metadata must be performed atomically.
   */
  enum intr_level old_level = intr_disable();
  ASSERT (lock != NULL);
  ASSERT (lock_held_by_current_thread (lock));

  struct thread *t = thread_current();

  // release the lock
  lock->holder = NULL;

  if (priority_donation_enabled) {
    /*
     * Find the lock_priority corresponding to the lock released, then remove
     * the lock_priority from the executing thread's priority list, and reset
     * it with zero bits.
     */
    struct lock_priority *released_lock_priority = find_lock_priority(&t->lock_priority_list, lock);
    list_remove(&released_lock_priority->elem);
    memset(released_lock_priority, 0, sizeof(struct lock_priority));
  }

  /*
   * Wake up a waiter thread if any; and if the waiter thread has a higher
   * priority, yield the processor.
   * 
   * sema_up() must occur before the current thread gets its new priority,
   * because if the order is flipped and the executing thread gets a lower
   * priority, it will yield the processor without moving its donor onto the
   * ready-list. This means that the donor cannot resume execution until the
   * current thread gets scheduled again based on its decreased priority. This
   * defeats the purpose of priority donation.
   * 
   * On the other hand, if we wake up a waiter thread before updating the current
   * thread's effective priority, then the current thread likely has the same
   * priority as the highest waiter thread, and so wouldn't need to yield the
   * processor. It will hence return from sema_up(), update its effective priority,
   * and yield its processor then.
   */
  sema_up(&lock->semaphore);

  if (priority_donation_enabled) {
    int old_priority = thread_get_priority();
    int new_priority;
    barrier();

    /*
     * The executing thread's new effective priority should be the highest priority
     * among the original_priority plus all lock_priorities it still holds. If
     * it has released all locks and hence no longer have any lock_priorities,
     * its original priority will be the new effective priority.
     * 
     * If the new effective priority is lower than the old one, the current
     * thread should immediately yield the processor.
     */
    struct list_elem *highest_priority_elem = list_max(&t->lock_priority_list, default_list_less_func, lock_priority_convert_func);
    if (highest_priority_elem != list_end(&t->lock_priority_list)) {
      int highest_lock_priority = list_entry(highest_priority_elem, struct lock_priority, elem)->priority;
      new_priority = highest_lock_priority >= t->original_priority ? highest_lock_priority : t->original_priority;
    } else {
      new_priority = t->original_priority;
    }

    t->priority = new_priority;
    if (old_priority > new_priority)
      thread_yield();
  }

  intr_set_level(old_level);
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

/* A waiter of a condition variable.

   Note: no idea why we have to put (struct condition_waiter).elem, instead of
   (struct thread).elem, into (struct condition).waiters. The former is simpler,
   does not conflict with the ready-list, and can save the definition of "struct
   thread" here. But it's kept that way, lest the simpler implementation introduces
   some unknown bugs.
 */
struct condition_waiter
  {
    struct list_elem elem;              /* List element. */
    struct thread *t;                   /* TCB of the waiting thread. */
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

/* Atomically releases LOCK and waits for COND to be signaled by some thread.
   The caller must have held the LOCK, and the LOCK will be re-acquired after
   COND is signaled.

   The monitor implemented by this function is "Mesa" style, not "Hoare" style:
   that is, sending and receiving a signal are not an atomic operation. Thus,
   typically the awakened thread must recheck the condition after the wait
   completes and, if necessary, wait again. However, the Pintos implementation
   of condition variable is a useless dummy, because it doesn't contain the
   value of the variable, which is required for the awaken thread to determine
   whether the condition it cares about is true. In short, the thread calling
   this function technically can't check if the condition is still true after
   being awaken.

   A given condition variable is associated with only a single lock, but one
   lock may be associated with any number of condition variables. That is,
   there is a one-to-many mapping from locks to condition variables.

   This function may sleep, so it must not be called within an interrupt handler.
   This function may be called with interrupts disabled, but interrupts will be
   turned back on once the thread sleeps and switches to another thread.
 */
void
cond_wait (struct condition *cond, struct lock *lock)
{
  enum intr_level old_level = intr_disable();

  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));

  // represents the waiting thread
  struct condition_waiter waiter;
  waiter.t = thread_current();
  sema_init (&waiter.semaphore, 0);

  list_push_back (&cond->waiters, &waiter.elem);
  lock_release (lock);
  sema_down (&waiter.semaphore);
  lock_acquire (lock);

  intr_set_level(old_level);
}

// Converts a (struct condition_waiter).elem to the waiter thread's effective priority.
static int waiter_priority_convert_func(const struct list_elem *e) {
  return list_entry(e, struct condition_waiter, elem)->t->priority;
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
  enum intr_level old_level = intr_disable();

  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));

  if (!list_empty (&cond->waiters)) {
    struct list_elem *waiter_elem = list_max(&cond->waiters, default_list_less_func, waiter_priority_convert_func);
    list_remove(waiter_elem);
    struct condition_waiter *waiter = list_entry(waiter_elem, struct condition_waiter, elem);
    sema_up(&waiter->semaphore);
  }

  intr_set_level(old_level);
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
