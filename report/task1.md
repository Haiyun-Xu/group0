Background & context:
- there are three ways to suspend the current thread: thread_yield(), thread_block(), and thread_exit(). thread_yield() simply changes the caller thread status to READY, places the caller thread on the ready-queue, and the caller thread could be re-scheduled immediately. thread_block() changes the caller thread status to BLOCKED, but does not place the caller thread on any list; the caller must therefore have already place the TCB on some list, otherwise the current thread will be lost and cannot be scheduled again - this is usually done by a synchronization primitive. thread_exit() changes the caller thread to DYING, and schedules the next thread; the next thread will free the previous thread in thread_schedule_tail();
- timer ticks are implemented as external interrupts, and the interrupt vector number is 0x20. For every clock tick, an external interrupt is triggered, and the interrupt handler calls the timer_interrupt() function;

Requirements:
- if X <= 0, timer_sleep() should return immediately;
- timer_sleep() should not busy-wait, and instead should suspend the caller thread for a minimum of X ticks. After the ticks, the caller thread would only be awaken if there is no other non-idle thread to run;
- you may not acquire locks while executing timer_interrupt(), as it could potentially suspend the thread and skip over the entire clock tick;
- the code in timer_interrupt() should be as fast as possible, because it's executed every clock tick. You can do some pre-computation outside of the interrupt handler to minimize the computation there;

Solution & design:
- the only two function that accesses the sleeping list are thread_sleep() and thread_tick(). As long as thread_sleep() disables interrupt, the timer interrupt will be ignored, and so thread_tick() will not execute. Similarly, when thread_tick() executes, external interrupt has already occurred, so thread_sleep() could not have been interrupted during execution. This guarantees atomic operation on the sleeping list without a lock;
- the TCB will contain two new elements, "int64_t awake_point" and "struct list_elem sleep_list_elem";
- timer_sleep(): disables interrupt, calculate and populate the tick at which the thread should be awaken, then call thread_sleep(). When thread_sleep() returns, enable the interrupt and return;
- thread_sleep(): changes the current thread to BLOCKED, but does not need to take it off from any list - there's no running list. Puts it into the sleeping list, call schedule() to switch to a new thread. When schedule() returns, return to timer_sleep();
- timer_interrupt(): calls thread_tick(). When thread_tick() returns, simply return to the interrupt handler;
- thread_tick(): for each of the sleeping threads, check whether it should be awaken at the current clock tick. If so, remove the thread from the sleeping list, changes it to READY, and put it on the ready-list;

Tips:
- you can use `--realtime` flag to let Pintos use wall clock time. Otherwise Pintos time will pass much faster. There are TIMER_FREQ ticks in one second;
