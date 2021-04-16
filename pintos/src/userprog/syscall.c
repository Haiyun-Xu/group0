#include "devices/shutdown.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/descriptor.h"
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
 * If the pointer is invalid, the process is terminated.
 */
static void check_valid_pointer(const void *pointer, unsigned size, bool check_writable) {
  /*
   * invalidate NULL pointers, pointers to kernel memory space, or data
   * structure that grows into the kernel memory space
   */
  if (
    pointer == NULL
    || !is_user_vaddr(pointer)
    || size > (unsigned) ((uint8_t *) PHYS_BASE - (uint8_t *) pointer)
  )
    thread_exit(-1);
  
  // find the starting and ending user page
  uint8_t *current_user_page = pg_round_down(pointer);
  uint8_t *end_user_page = pg_round_down((uint8_t *)pointer + size - 1);
  struct thread *t = thread_current();

  // check that each of the user page is allocated (and writable if asked)
  for (; current_user_page <= end_user_page; current_user_page += PGSIZE) {
    if (!is_user_page_present(t->pagedir, current_user_page, check_writable))
     thread_exit(-1);
  }
}

/*
 * Check if the given string is valid. If the string is invalid, the process is
 * terminated.
 */
static void check_valid_string(const char **pointer) {
  // check that the address of the string pointer is valid
  if (pointer == NULL)
    thread_exit(-1);
  check_valid_pointer((void *) pointer, sizeof(char *), false);
  
  /*
   * last_page must be different from current_page initially; use an address
   * that's not a page address to guarantee this difference
   */
  const char *string = *pointer;
  uint8_t *last_page = (uint8_t *) 0x1;
  uint8_t *current_page = pg_round_down(string);

  // check the address of each character, until the null-char is encountered
  while (true) {
    if (last_page != current_page)
      check_valid_pointer(string, sizeof(char), false);
    
    // if the pointer was valid and points to a null-char, the entire string is valid
    if (*string == '\0')
      return;
    
    // otherwise, move onto the next character
    last_page = current_page;
    current_page = pg_round_down(++string);
  };
}

/* Project 1 task 2 */
static int practice (int i) {
  return ++i;
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

/* Project 1 task 3 */
static bool create (const char *file, unsigned initial_size) {
  return fd_create(file, initial_size);
}
static bool remove (const char *file) {
  return fd_remove(file);
}
static int open (const char *file) {
  return fd_open(file);
}
static int filesize (int fd) {
  return fd_filesize(fd);
}
static int read (int fd, void *buffer, unsigned size) {
  return fd_read(fd, buffer, size);
}
static int write (int fd, const void *buffer, unsigned size) {
  return fd_write(fd, buffer, size);
}
static void seek (int fd, unsigned position) {
  return fd_seek(fd, position);
}
static unsigned tell (int fd) {
  return fd_tell(fd);
}
static void close (int fd) {
  return fd_close(fd);
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
  check_valid_pointer(args, sizeof(*args), false);

  /*
   * validate the rest of the arguments, then invoke the respective syscall based
   * on the syscall number.
   */
  switch (args[0]) {
    case SYS_PRACTICE:
      check_valid_pointer(&args[1], sizeof(int), false);
      f->eax = (uint32_t) practice(*((int *)&args[1]));
      break;
    case SYS_HALT:
      halt();
      break; // not reached
    case SYS_EXIT:
      check_valid_pointer(&args[1], sizeof(int), false);
      exit(*((int *)&args[1]));
      break; // not reached
    case SYS_EXEC:
      check_valid_string((const char **) &args[1]);
      f->eax = (uint32_t) exec(*((char **)&args[1]));
      break;
    case SYS_WAIT:
      check_valid_pointer(&args[1], sizeof(pid_t), false);
      f->eax = (uint32_t) wait(*((pid_t *)&args[1]));
      break;
    case SYS_CREATE:
      check_valid_string((const char**) &args[1]);
      check_valid_pointer(&args[2], sizeof(unsigned), false);
      f->eax = (uint32_t) create(*((char **)&args[1]), *((unsigned *)&args[2]));
      break;
    case SYS_REMOVE:
      check_valid_string((const char**) &args[1]);
      f->eax = (uint32_t) remove(*((char **)&args[1]));
      break;
    case SYS_OPEN:
      check_valid_string((const char**) &args[1]);
      f->eax = (uint32_t) open(*((char **)&args[1]));
      break;
    case SYS_FILESIZE:
      check_valid_pointer(&args[1], sizeof(int), false);
      f->eax = (uint32_t) filesize(*((int *)&args[1]));
      break;
    case SYS_READ: // static int read (int fd, void *buffer, unsigned size)
      check_valid_pointer(&args[1], sizeof(int), false);
      check_valid_pointer(&args[2], sizeof(void *), false);
      check_valid_pointer(&args[3], sizeof(unsigned), false);
      check_valid_pointer(*((void **)&args[2]), *((unsigned *)&args[3]), true);
      f->eax = (uint32_t) read(*((int *)&args[1]), *((void **)&args[2]), *((unsigned *)&args[3]));
      break;
    case SYS_WRITE: // write (int fd, const void *buffer, unsigned size)
      check_valid_pointer(&args[1], sizeof(int), false);
      check_valid_pointer(&args[2], sizeof(void *), false);
      check_valid_pointer(&args[3], sizeof(unsigned), false);
      check_valid_pointer(*((void **)&args[2]), *((unsigned *)&args[3]), false);
      f->eax = (uint32_t) write(*((int *)&args[1]), *((const void **)&args[2]), *((unsigned *)&args[3]));
      break;
    case SYS_SEEK:
      check_valid_pointer(&args[1], sizeof(int), false);
      check_valid_pointer(&args[2], sizeof(unsigned), false);
      seek(*((int *)&args[1]), *((unsigned *)&args[2]));
      break;
    case SYS_TELL:
      check_valid_pointer(&args[1], sizeof(int), false);
      f->eax = (uint32_t) tell(*((int *)&args[1]));
      break;
    case SYS_CLOSE:
      check_valid_pointer(&args[1], sizeof(int), false);
      close(*((int *)&args[1]));
      break;
    default:
      thread_exit(-1);
      break; // not reached
  }
}
