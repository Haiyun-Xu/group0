#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "list.h"
#include "threads/synch.h"
#include "threads/thread.h"

/* Process identifier. */
typedef int pid_t;
#define PID_ERROR ((pid_t) -1)

// a structure used by the parent process to track child processes's exit status
struct process_status {
  bool in_use;
  struct semaphore sema;
  pid_t pid;
  struct list_elem subprocess_list_elem;
  int exit_status;
  char padding[0];
};

void process_init(void);
pid_t process_execute (const char *file_name);
int process_wait (pid_t pid);
void process_exit (int exit_status);
void process_activate (void);

#endif /* userprog/process.h */
