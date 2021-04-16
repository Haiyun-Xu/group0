## Syscall argument verification

The in-user-space verification needs to be done by the interrupt handler, because: 1) user thread is not supposed to reference kernel address; 2) the interrupt handler is a kernel thread, so it can dereference the user-given kernel address without triggering a page fault;

The syscall arguments were pushed onto the user stack, so we'll need the user stack pointer in order to retrieve the arguments. The user stack pointer is extracted from the interrupt frame and must be validated; so do the arguments on the user stack.

When validating a syscall argument's address or a pointer argument, we must check that:
1. the address is not NULL;
2. the address is in user space;
3. the argument is no larger than the space between its address and PHYS_BASE;
4. every memory page that the argument touches must already be allocated in the user virtual memory space;
5. if the argument is for storing data, then every memory page that it touches must be writable;

---

## How does a syscall switch from user context to kernel context

User program calls the syscall library function in lib\user\syscall.c, which triggers a software/internal interrupt. The CPU pushes user thread states onto the kernel stack, the interrupt routines push other meta data (including the interrupt vector code), then the kernel thread jump to intr_entry in threads\intr-stubs.S. The intr_entry pushes more registers onto the interrupt frame, and calls intr_handler() in threads\interrupt.c, which finally calls the specific handler function tied to the interrupt vector number. This should take us to syscall_handler() in userprog\syscall.c.

---

## How does a process get created, what memory pages are allocated, when are they released, and what system states are modified <a href="#process_creation"></a>

1. process_execute() called in _parent kernel context_: it __allocates the temporary kernel page to store the command line__;
2. thread_create() called in _parent kernel context_: it __allocates the child process kernel page__, initializes the TCB on the kernel page, and __pushes the TCB onto the all-threads list__. It then place the child kernel thread on the ready list, but it is unknown when the thread will run or whether the child user thread will execute;
3. switch_thread(), switch_entry(), then thread_schedule_tail() are called in _child kernel context_: if the parent thread is set to DYING status, __thread_schedule_tail() will free the parent kernel page__;
4. kernel_thread(), then start_process() is called in _child kernel context_: start_process() __takes the temporary kernel page as argument__, and __load the program executable into user memory pages__. It then __frees the temporary kernel page__, and switches context into the child user thread;
5. thread_exit() is called in the _current kernel context_: kernel_thread(), start_process(), and exit() syscall can call thread_exit() if the current kernel thread behaves illegaly, fails to load user executables, or finishes execution. It destroys the current process's page directory, __removes the TCB from the all-threads list__, sets the current thread to DYING status, and calls schedule();
6. schedule() is called in the _current kernel context_: it switches to the next kernel thread and calls thread_schedule_tail(), which then __frees the curent process's kernel page__;

---

## exec() implementation

### Requirement:

The exec() syscall should start a new process to run the executable given in the command. The function should return -1 if the new process fails to execute for whatever reason, and should not return until it is confirmed that the new process has succeeded/failed to execute. This requires the use of synchronization primitive between the parent and child process.

### Consideration:

We can synchronize the parent and child process with a semaphore. Since __the parent kernel thread shouldn't return untill it is confirmed whether the child user thread can execute__, it's clear that the semaphore should only be incremented when the child kernel thread has successfully completed loading the executables - in other words, __sema_up() should be called at the end of start_process()__, right before switching into user context.

Due to the existing "temporary" semaphore being a static variable accessible by all kernel threads, there's no isolation between kernel threads calling process_execute(). Consider the following scenario: 1) a parent kernel thread initializes the semaphore, calls sema_down() and gets suspended, 2) the child kernel thread gets scheduled, finishes loading and calls sema_up(), then puts the parent kernel thread on the ready list, 3) since there's no guarantee that the parent kernel thread would get scheduled immediately, it is entirely possible that the child user thread calls exec() to create a grand-child kernel thread, resetting the semaphore to 0 and erasing the signal meant for its parent kernel thread. Therefore, __the semaphore must be stored at a location only accessible by the parent and child kernel threads__.

The semaphore only signals the end of child kernel thread's initialization, and does not reveal whether the child user thread will successfully execute. Therefore, __we need another variable, accessible only by the parent and child kernel threads, to signal whether the child user thread can execute.__ This variable can be a pid, and should be set to -1 if the child user thread cannot execute.

From the [process creation analysis above](#process_creation), we see that the semaphore and the pid variable cannot be placed in the child TCB (or its kernel page). The reason is that, if the child kernel thread fails to load the user executables and exits, a new thread can get scheduled immediately and then release the child kernel page (containing the semaphore and pid) right away. There is no guarantee that the parent kernel thread, suspended and waiting for the signal from the child kernel thread, gets to be scheduled before the new thread. On the other hand, it is easy to see that the semaphore and pid shouldn't be placed in child process's user memory - they can get tampered immediately.

The only option left is therefore to place the semaphore and pid in parent kernel memory This is simpler than using parent user memory, which is technically valid as long as the parent kernel thread doesn't switch back to user context until the synchronization is complete. In fact, the simplest design would be to fit the semaphore and pid into the temporary page storing the command line. Some changes certainly need to be made, and the design are as follows:
1. the semaphore and pid are placed at the top of the temporary page, whereas the command line at the bottom. We should check if the command line is too long first, before copying it into the temporary page;
2. since the temporary kernel page is created by process_execute(), we should also initialize the semaphore and pid in process_execute();
3. as for where sema_down() should be called to wait for the child process's signal, it's preferable to also perform that logic in process_execute(). The main reason is that, thread_create() is a general function that can be given different starting functions and arguments, and so there's no guarantee that there are synchronization primitives in the arguments or that the starting function will call sema_up() on the synchronization primitive. Hence, thread_create() should stay ignorant of the behavior of the given function and any synchronization that might occur;
4. the new start_process() should update the pid based on the status of the child user thread, and set it to -1 if it failed for whatever reason. Only then can it call sema_up() and allow the parent kernel thread to be scheduled;
5. since the semaphore and pid are now in the temporary page, which should stay in use until the parent kernel thread resumes, start_process() should no longer free the temporary page. Instead, this will be done by process_execute().

---

## wait() and exit() implementation

### Requirement:

The wait(pid_t pid) syscall must have the following behavior:
1. the argument pid must be a direct child process of the caller process, and cannot be a grandchild process;
2. when a child process exits earlier than the grandchild processes, the grandchild processes do not get their grandparent process reassigned as the new parent process;
3. if process pid is still alive, the calling process should wait until it exits;
4. once process pid exits, the calling process should resume and be able to get process pid's exit status;
5. the calling process should be able to get process pid's exit status, even if process pid has already exited;
6. if process pid is not a child process of the calling process, or if process pid was terminated involuntarily by the kernel (page fault, syscall failure, etc), then wait() should return -1;
7. if process pid has already been waited on, then a second time calling wait() on that pid will return -1;

Some additional requirments are:
1. a parent process may wait for its children processes in any order, or not wait for them at all. The children processes must be able to release their resources, regardless if they are waited for or if they exited before their parent.  
  a. if the release of the child process resource is dependent on the parent process waiting for it, then the resource will not be released if the parent does not wait or have exited before the child;  
  b. thus, the child process resources must be released independent of the waiting mechanism, because the only other option is to run a periodic cleanup job that frees the resources of children processes whose parent either didn't wait or have exited before them;  
  c. this also means that the child process exit status must not be stored in the child process. Since the parent process needs to keep track of all its children processes and need to be able to retrieve their exit status after they've exited, these info should be stored in the parent process;  
2. Pintos waits on the initial kernel thread by calling process_wait(), whereas normal processes calls the syscall wait(). Pintos must not exit before the initial kernel thread, so we should implement all the above features in process_wait(), and then implement wait() in terms of process_wait().

### Consideration:

Each PCB should now have two addditional properties:
1. pointer to a list of child process monitors;
2. pointer to its own monitor in the parent process;

The pointer to the monitor is passed to start_process() in the temporary kernel page. Whenever the process exits, either voluntarily or involuntarily, it should update the exit status in its monitor. The only exception is the initial PINTOS process, which has no parent process and does not need to report its exit status, and so will get a NULL pointer.

The list of child process monitors should be on a separate parent kernel page than the main parent kernel page, so as to avoid competing against the kernel stack for memory. This additional page should be created either in thread_create() or start_process(), because the list must be initialized before the child user thread runs (in case the user thread creates a new process right away). If the page were to be created in process_execute(), process_execute() may get interrupted and the child process might be scheduled immediately to run, so there's no guarantee in this case that the list can be initialized before the child user thread runs.
- considering that thread_create() is called elsewhere and should not be aware of process-level logic, we should implement the list page allocation and list initialization in start_process();
- when a new child process is created, the parent process should allocate the child monitor first. In case where there's no more space on the list page, the child process should not be created;
- the initial Pintos process also gets its page and list of child process monitors created;

Since a process's own monitor lives in the parent process memory, and that the parent process could exit earlier than the child, it's possible that the pointer to the monitor has become invalid by the time the child process dereferences it. Therefore, the child process must first verify if the monitor is still accessible, which requires checking the bit free map. Since this verification and the monitor modification must be atomic, we introduce a universal lock that gives exclusive access to one process at a time, preventing race conditions between parent and child processes.

### Behavior description:

When Pintos process starts:  
(in the pintos kernel thread)
- the subprocess monitors page should be allocated from the kernel pool and zeroed. If it fails to allocate, kernel should panic;
- the subprocess monitors list should be initialized;
- in the Pintos kernel thread, the pstatus pointer should set to NULL for the Pintos process;

When a normal child process gets created:  
(in the parent kernel thread)
- we should first allocate and initialize a new subprocess status monitor on the parent process's subprocess monitors page. If it fails to allocate, the child process should not be created;
- we should pass the command line, pid and creation semaphore to the child process for starting the child user thread and synchronizing. We should also pass it the pstatus pointer to the subprocess status monitor we allocated, for waiting for the child process to exit and retrieving its exit status;
- we should wait until the creation of the child user thread, then examine the pid value: if it's a normal pid, then the subprocess status monitor's pid field should be updated, and the monitor should be pushed into the list; otherwise if it's -1, then the subprocess status should be freed. The temporary kernel page should be freed regardless;

(in the child kernel thread)
- the subprocess monitors page should be allocated from the kernel pool and zeroed, and should be populated into the child process PCB. If it fails to allocate, the child process should fail and not execute the user thread;
- the subprocess monitors list should be initialized;
- whether the allocation and image loading are successful or not, the value at the pid pointer should be populated accordingly, and the creation semaphore should be signaled;
- if the child user thread can execute successfully, the pstatus pointer given by the parent process should be populated into the PCB;

When a normal process exits;
- when a process exits, it must both access its parent's subprocess monitors page and free its own subprocess monitors page, and so could run into race conditions with its page-freeing parent or its page-accessing child. To prevent the race condition, the exiting process must first acquire the process_exit_lock, so that it is the only exiting process in the entire OS;

(as a child process)
- we should report the exit_status to parent process and signal it through the pstatus pointer in the PCB;

(as a parent process)
- we should free the subprocess monitors page. Even though the subprocess monitors list is on the TCP and will be nullified when the TCB is freed, we should still empty the list to preserve its state;

(as the waiting parent process)
- we should find the subprocess status monitor corresponding to process pid, then wait for it to exit through the semaphore. Once the execution resumes, we should cache the exit_status, remove the monitor from the list, and free the monitor memory;

When the Pintos process exits;  
(as a child process)
- the Pintos process has no parent, so its pstatus pointer is set to NULL, and it doesn't need to report its exit status;

(as a parent process)
- we should free the subprocess monitors page. Even though the subprocess monitors list is on the TCP and will be nullified when the TCB is freed, we should still empty the list to preserve its state;
