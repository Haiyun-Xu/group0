#include "devices/shutdown.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "userprog/syscall.h"

static void syscall_handler (struct intr_frame *);

void
syscall_init (void)
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

/*
 * Check if the given pointer is a non-NULL address in the user virtual memory.
 * It checks the address of both the starting byte as well as the ending byte.
 */
static bool is_valid_pointer(const void *pointer, uint8_t size, bool check_writable) {
  /*
   * invalidate NULL pointers, pointers to kernel memory space, or data
   * structure that grows into the kernel memory space
   */
  if (
    pointer == NULL
    || !is_user_vaddr(pointer)
    || size > (uint8_t *) PHYS_BASE - (uint8_t *) pointer
  )
    return false;
  
  // find the starting and ending user page
  uint8_t *current_user_page = pg_round_down(pointer);
  uint8_t *end_user_page = pg_round_down((uint8_t *)pointer + size - 1);
  struct thread *t = thread_current();

  // check that each of the user page is allocated (and writable if asked)
  for (; current_user_page <= end_user_page; current_user_page += PGSIZE) {
    if (!is_user_page_present(t->pagedir, current_user_page, check_writable))
      return false;
  }
  return true;
}

/*
 * Check if the given string is valid.
 */
static bool is_valid_string(const char *pointer) {
  if (pointer == NULL)
    return false;
  
  /*
   * last_page must be different from current_page initially; use an address
   * that's not a page address to guarantee this difference
   */
  uint8_t *last_page = (uint8_t *) 0x1;
  uint8_t *current_page = pg_round_down(pointer);

  // check the address of each character, until the null-char is encountered
  do {
    if (last_page != current_page && !is_valid_pointer(pointer, sizeof(char), false))
      return false;
    
    last_page = current_page;
    current_page = pg_round_down(pointer + 1);
  } while (*pointer++ != '\0');

  return true;
}

static void halt (void) {
  shutdown_power_off();
}

static void exit (int status) {
  thread_exit(status);
}

static pid_t exec (const char *file) {
  return process_execute(file);
}

static int wait (pid_t pid) {
  return process_wait(pid);
}

static int practice (int i) {
  return i++;
}

static void
syscall_handler (struct intr_frame *f)
{
  /*
   * As the syscall handler, any value manipulatable by the user code must be
   * validated. The only guarantee we have is that f->esp is the value in the
   * ESP register when the interrupt occurred (because it's saved by the
   * processor, so user code couldn't have tampered that). We don't know if
   * esp is a valid stack pointer, nor if there's arguments above the stack
   * pointer, nor if those arguments' address or value are correct.
   */
  uint32_t *args = (uint32_t *) f->esp;

  /*
   * validate that the entire syscall number pointed by args is in the user
   * memory. Technically, the arguments should be written onto the user stack,
   * and so args should be on a writable user page, but since we're only reading
   * it here, it doesn't matter
   */
  if (!is_valid_pointer(args, sizeof(*args), false))
    thread_exit(-1);

  /*
   * validate the rest of the arguments, then invoke the respective syscall based
   * on the syscall number.
   */
  switch (args[0]) {
    case SYS_EXIT:
      if (!is_valid_pointer(&args[1], sizeof(int), false))
        thread_exit(-1);
      exit(args[1]);
      break; // not reached

    case SYS_HALT:
      halt();
      break; // not reached

    case SYS_EXEC:
      if(!is_valid_string((char *) &args[1]))
        thread_exit(-1);
      f->eax = (uint32_t) exec((char *) args[1]);
      break;

    case SYS_WAIT:
      if (!is_valid_pointer(&args[1], sizeof(pid_t), false))
        thread_exit(-1);
      f->eax = (uint32_t) wait(args[1]);
      break;

    case SYS_PRACTICE:
      if (!is_valid_pointer(&args[1], sizeof(int), false))
        thread_exit(-1);
      f->eax = (uint32_t) practice(args[1]);
      break;

    default:
      thread_exit(-1);
      break; // not reached
  }
}
