#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/descriptor.h"
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

static thread_func start_process NO_RETURN;
static struct lock process_exit_lock;

static bool setup_subprocess_list(void);
static void parse_temporary_kpage(
  const char *temp_kpage,
  pid_t **pid_ptr,
  struct semaphore **sema_ptr,
  struct process_status ***pstatus
);
static struct process_status *get_new_subprocess_monitor(void);
static struct process_status *get_subprocess_monitor(pid_t pid);
static bool load (const char *cmdline, void (**eip) (void), void **esp);

/*
 * Initialize the original Pintos process PCB.
 */
void
process_init(void) {
  /*
   * The pstatus is set to NULL because the original Pintos process doesn't have
   * a parent and doesn't need to report its exit status. Once the subprocess
   * status monitors are setup, initialize the process lock, which is used for
   * synchronization between parent and child processes during access to the
   * parent's subprocess status monitors list.
   */
  thread_current()->pstatus = NULL;
  if (!setup_subprocess_list())
    PANIC("process_init: failed to create subprocess list");
  lock_init(&process_exit_lock);
}

/*
 * Starts a new child process to run a user program loaded from FILENAME.
 * The new process may be scheduled (and may even exit) before process_execute()
 * returns. This function waits until it is confirmed whether the new process
 * can execute. If the new process can execute, this function returns the new
 * process's thread id; otherwise if the new process cannot execute for whatever
 * reason, PID_ERROR is returned.
 */
pid_t
process_execute (const char *file_name)
{
  /* 
   * Allocate a teporary kernel page for the new process, and copy the file_name
   * from the caller's memory (could be kernel or user) into it. This is to
   * prevent race condition between the parent and child process, if they both
   * acess/modify the file_name.
   */
  char *temp_kpage = palloc_get_page(PAL_ZERO);
  if (temp_kpage == NULL)
    return PID_ERROR;
  
  /*
   * The temporary kernel page contains the command line arguments at the bottom,
   * as well as the pid variable and synchronization semaphore at the top. It
   * should be freed when it is confirmed whether the child process can execute.
   */
  char *file_name_copy = temp_kpage;
  pid_t *pid = NULL;
  struct semaphore *sema = NULL;
  struct process_status **pstatus = NULL;
  parse_temporary_kpage(temp_kpage, &pid, &sema, &pstatus);

  *pid = PID_ERROR;
  sema_init(sema, 0);
  *pstatus = NULL;
  size_t file_name_max_length = PGSIZE - sizeof(pid_t) - sizeof(struct semaphore) - sizeof(struct process_status);
  
  /*
   * only create the new process if the command line can fit into the temporary
   * kernel page and there's space on the list for a new subprocess monitor;
   * otherwise, skip to completion.
   */
  if (strlcpy(file_name_copy, file_name, file_name_max_length) < file_name_max_length) {
    *pstatus = get_new_subprocess_monitor();
    if (*pstatus != NULL) {
      /*
      * create a new process at default priority to execute the given program,
      * then wait until the new process is ready/fails to execute
      */
      thread_create(file_name_copy, PRI_DEFAULT, start_process, file_name_copy);
      sema_down(sema);
    }
  }

  /**
   * by now, pid should point to the new process's thread id, but it must be
   * cached first, because the temporary kernel page needs to be released. The
   * temporary kernel page must be freed after the allocated subprocess status
   * monitor is either reset or updated, because the pointer to the monitor is
   * stored on the temporary kernel page.
   * 
   * free the temporary kernel page, then depending on whether the child process
   * was created successfully, either reset the subprocess status monitor, or
   * add it to the calling process's list.
   */
  pid_t result = *pid;
  if (result == PID_ERROR) {
    memset(*pstatus, 0, sizeof(struct process_status));
  } else {
    (*pstatus)->pid = result;
    list_push_back(&(thread_current()->subprocess_status_list), &(*pstatus)->subprocess_list_elem);
  }
  palloc_free_page(temp_kpage);

  return result;
}

/*
 * Entry point of a new process's kernel thread. This function loads the program
 * executable, signals the parent process to resume, and decide whether to switch
 * into user context and executes the user program.
 */
static void
start_process (void *temp_kpage)
{
  /*
   * We are now in the child process's kernel thread. The interrupt frame if_
   * is on the child process kernel page (see thread.c:thread_create()), but
   * temp_kpage is a temporary kernel page created by the parent process, and
   * will be freed by the parent process once it's signaled to resume.
   */
  struct intr_frame if_;
  struct thread *t = thread_current();
  
  char *file_name = (char *) temp_kpage;
  pid_t *pid = NULL;
  struct semaphore *sema = NULL;
  struct process_status **pstatus = NULL;
  parse_temporary_kpage((char *) temp_kpage, &pid, &sema, &pstatus);

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;

  /*
   * Setup the page and list of subprocess status monitors, and the page of file
   * structures. If those are successful, load the program executables. The file
   * structures page must be setup before loading the executable, because the
   * "struct file" of the process's own executable needs to be stored on the page.
   * 
   * Initialize the pstatus pointer to NULL, because if either the page setup or
   * the executable loading failed, the child process is considered a failure,
   * and the parent process will free the allocated subprocess status monitor.
   * In order to prevent the race condition, where the child process reports its
   * exit status after the parent process frees the monitor, the child process's
   * pstatus pointer is first set to NULL, and only set to the actual value when
   * we confirm that the child user thread can execute.
   */
  bool successful = false;
  if (setup_subprocess_list() && setup_file_descriptors()) {
    successful = load(file_name, &if_.eip, &if_.esp);
  }
  t->pstatus = NULL;

  /*
   * if both subprocess status monitors and the program executable were setup
   * successfully, report the child process id to the parent process; otherwise,
   * report that it failed, and terminate the thread.
   */
  *pid = successful ? (pid_t) t->tid : PID_ERROR;
  sema_up(sema);
  if (!successful)
    thread_exit(-1);
  
  // update the process pstatus pointer to the actual value 
  t->pstatus = *pstatus;

  /*
   * Start the user process by simulating a return from an interrupt, which is
   * implemented by intr_exit (in threads/intr-stubs.S). Note that intr_exit
   * assumes that it follows an intr_entry (also in intr-stubs.S) and thus has
   * the interrupt frame (containing the "interruped" user thread's register
   * values) on the stack. We can therefore point the ESP register to if_ before
   * jumping to intr_exit, and make it load the if_ values into the register as
   * it context switches into user thread.
   */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/*
 * Waits for subprocess pid to exit and returns its exit status. Returns -1 if
 * any of the following occurs:
 * 1) process pid does not exist or is not a direct child of the calling process;
 * 2) process pid was terminated by the kernel (i.e. killed due to an exception);
 * 3) process_wait() has already ben called on process pid;
 */
int
process_wait (pid_t pid)
{
  /*
   * it is possible that the caller used process_wait(process_execute()), in
   * which case pid could be -1. In that case, since process pid was never
   * created, return -1 as if it was killed by the kernel.
   */
  if (pid == PID_ERROR)
    return -1;

  struct process_status *subprocess_status = get_subprocess_monitor(pid);
  if (subprocess_status == NULL)
    return -1;
  
  /*
   * wait until the process pid has exited, then remove it from the list of
   * subprocess status monitors, and free the monitor for the next usages.
   */
  sema_down(&subprocess_status->sema);
  int exit_status = subprocess_status->exit_status;
  list_remove(&subprocess_status->subprocess_list_elem);
  memset(subprocess_status, 0, sizeof(struct process_status));
  return exit_status;
}

/* Free the current process's resources. */
void
process_exit (int exit_status)
{
  /*
   * The current process reports its exit status to its parent process, signals
   * that it's exiting, then frees its own subprocess status monitors page.
   * 
   * There are three edge cases here:
   * 1) Pintos's initial process (0th) does not have a parent process, nor does
   * it need to report its exit status, so its pstatus pointer is NULL;
   * 2) if the current process never succeeded in executing the user thread, then
   * it's considered a failed process and shouldn't deference its pstatus pointer
   * (which is also set to NULL);
   * 3) if the parent process exited earlier, the pstatus would be inaccessible,
   * and the current process wouldn't need to report its exit status;
   *
   * Therefore, we must check that the pstatus pointer isn't NULL, and that it's
   * on an allocated kernel page, before we can dereference it.
   * 
   * Before checking whether the pstatus pointer is accessible or releasing the
   * process resource, we must acquire the universal process_exit_lock, so as to
   * prevent the race condition where the parent process releases the subprocess
   * monitors page at the same time as the child process tries to report its
   * exit status.
   */
  struct thread *t = thread_current ();
  lock_acquire(&process_exit_lock);

  if (t->pstatus != NULL && is_kaddr_valid(t->pstatus) && t->pstatus->in_use) {
    t->pstatus->exit_status = exit_status;
    sema_up(&t->pstatus->sema);
  }

  // empty the subprocess monitors list and release the page
  struct list *subprocess_status_list = &t->subprocess_status_list;
  for (
    struct list_elem *elem = list_begin(subprocess_status_list);
    elem != list_tail(subprocess_status_list);
    elem = list_next(elem)
  )
    list_remove(elem);
  palloc_free_page(t->subprocess_status_page);

  lock_release(&process_exit_lock);

  /*
   * close all of the process's file descriptors and free the descriptor page,
   * then close the process's executable to allow modifications.
   */
  close_all_file_descriptors();
  palloc_free_page(t->file_descriptor_page);
  close_process_executable();
  t->process_executable = NULL;

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  uint32_t *pd = t->pagedir;
  if (pd != NULL)
    {
      /* Correct ordering here is crucial.  We must set
         t->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      t->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
  
  printf("%s: exit(%d)\n", t->name, exit_status);
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/*
 * Setup the current process's list of subprocess status monitors. Allocates a
 * kernel page to store the list of subprocess status monitors, and initialize
 * the subprocess status monitor list.
 */
static bool
setup_subprocess_list(void) {
  void *page_addr = palloc_get_page(PAL_ZERO);
  if (page_addr == NULL)
    return false;

  struct thread *t = thread_current();
  t->subprocess_status_page = page_addr;
  list_init(&t->subprocess_status_list);
  return true;
}

/*
 * Parse the temporary kernel page pointer into its component pointers.
 */
static void
parse_temporary_kpage(
  const char *temp_kpage,
  pid_t **pid_ptr,
  struct semaphore **sema_ptr,
  struct process_status ***pstatus_ptr
) {
  // edge cases
  if (temp_kpage == NULL)
    return;
  
  *pid_ptr = (pid_t *) (temp_kpage + PGSIZE - sizeof(pid_t));
  *sema_ptr = (struct semaphore *) ((uint8_t *) *pid_ptr) - sizeof(struct semaphore);
  *pstatus_ptr = (struct process_status **) ((uint8_t *) *sema_ptr) - sizeof(struct process_status);
  return;
}

/*
 * Returns a new and initialized subprocess status monitor from the calling
 * process's subprocess monitors list. IF there's no space on the list, a null
 * pointer is returned.
 */
static struct process_status *get_new_subprocess_monitor() {
  struct thread *t = thread_current();
  struct process_status *monitor = (struct process_status *) t->subprocess_status_page;
  ASSERT(monitor != NULL);

  /*
   * iterate through all the subprocess status monitors on the page, initialize
   * and return the first one that is free
   */
  for (
    uint8_t *page_end = ((uint8_t *) t->subprocess_status_page) + PGSIZE;
    ((uint8_t *) monitor) + sizeof(struct process_status) <= page_end;
    monitor++
  ) {
    if (!monitor->in_use) {
      monitor->in_use = true;
      sema_init(&monitor->sema, 0);
      return monitor;
    }
  }

  return NULL;
}

/*
 * Returns the monitor associated with subprocess pid. If pid is not a direct
 * subprocess of the calling process, a null pointer is returned.
 */
static struct process_status *get_subprocess_monitor(pid_t pid) {
  /*
   * iterate through all the subprocess status monitors in the list, return the
   * one that corresponds to the given pid
   */
  struct list *subprocess_status_list = &(thread_current()->subprocess_status_list);
  for (
    struct list_elem *elem = list_begin(subprocess_status_list);
    elem != list_tail(subprocess_status_list);
    elem = list_next(elem)
  ) {
    struct process_status *subprocess_status = list_entry(elem, struct process_status, subprocess_list_elem);
    if (subprocess_status->pid == pid)
      return subprocess_status;
  }
  
  return NULL;
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

#define PROGRAM_NAME_MAX_LENGTH 64
static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);
static bool load_arguments(const char *argument, void **esp);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp)
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL)
    goto done;
  process_activate ();

  /* Extract the program name and open executable file. */
  char program_name[PROGRAM_NAME_MAX_LENGTH];
  int program_name_length = strcspn(file_name, " ");
  if (program_name_length >= PROGRAM_NAME_MAX_LENGTH) {
    printf("load: %s: program name must be <= 63 characters\n", file_name);
    goto done;
  }
  strlcpy(program_name, file_name, program_name_length + 1);

  /*
   * the process's own executable must be opened through this special function,
   * which prohibits any process from writing to the executable, until this
   * process exits.
   * 
   * The struct file of the executable should not be closable by the user program,and so should be stored in the PCB instead of in the process descriptors page.
   */
  file = open_process_executable(program_name);
  if (file == NULL) {
    printf ("load: %s: open failed\n", program_name);
    goto done;
  } else {
    t->process_executable = file;
  }

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024)
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done;
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++)
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type)
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file))
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack(esp))
    goto done;
  
  /*
   * At this point, user pages are allocated, the program image is loaded into
   * user memory, and the stack pointer is set to PHYS_BASE - 20, which is 12
   * bytes above a 4-word aligned address.
   * 
   * We can now copy the program call arguments into user memory, and push the
   * argv, argc, and return address onto the stack. Since the page directory
   * has been activated and the allocated user page has been added to the page
   * directory, user virtual memory can be directly referenced, and MMU will
   * do the conversion to physical memory address.
   */
  if (!load_arguments(file_name, esp))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /*
   * regardless if the load was successful, leave the file open, so that the
   * executable is not modifiable. Either way, when the process exits (due to
   * failure or normally), the executable will be closed.
   */
  return success;
}


/*
 * Load the program arguments and main() arguments onto the user stack. Upon
 * returning, the user stack will look like the following:
 * 
 * PHYS_BASE +==================================+
 *           | inital offset (20 Bytes)         |
 *           |----------------------------------|
 *           | program arguments                |
 *           |----------------------------------|
 *           | argument token pointers          |
 *           |----------------------------------|
 *           | (offset to align the args below) |
 *           |----------------------------------|
 *           | char *argv []                    |
 *           |----------------------------------|
 *           | int argc                         |
 *           |----------------------------------|
 *           | return address (just NULL)       |
 *           |----------------------------------| <= ESP, stack aligned (4-word aligned)
 *           | ...                              |
 * 1st page  +==================================+
 */
static bool load_arguments(const char *argument, void **esp) {
  // edge cases, allow argument but not %esp to be NULL
  if (argument == NULL)
    return true;
  else if (esp == NULL)
    return false;
  
  // converts the user virtual address to kernel virtual address
  struct thread *t = thread_current();
  void *stack_pointer = utok(t->pagedir, *esp);

  /*
   * shift the stack pointer to make room for the argument string.
   * 
   * putting an arbitrary limit on the length of the argument, because an exact
   * calculation is complicated: if the length of the argument (including the
   * null-char) is N, then there can be at most N/2 tokens in the argument, and
   * therefore we would want:
   *  USER_STACK_OFFSET_IN_BYTES + N + N/2 * sizeof(char*) + (4 * 3) <= PGSIZE,
   * so as to avoid allocating another user page.
   */
  size_t max_argument_length = 1024;
  size_t argument_length = 1 + strnlen(argument, max_argument_length);
  if (argument_length >= max_argument_length)
    return false;
  stack_pointer = (void *) ((char *) stack_pointer - argument_length);

  // copy the argument string into user stack
  strlcpy((char *) stack_pointer, argument, argument_length);
  char *user_argument = (char *) stack_pointer;

  /*
   * since the stack_pointer is an address of char**, it is more than a byte and
   * therefore must be alligned on words. Then, shift the stack_pointer to make
   * room for the token pointers.
   * 
   * The token pointers aray must end will a NULL pointer, so we reserve room
   * for it as well
   */
  size_t aligntment_offset = (((uint32_t) stack_pointer) % sizeof(void *));
  stack_pointer = (void *) ((uint32_t) stack_pointer - aligntment_offset);
  char *delimiters = " \t";
  int argc = strtok_c(user_argument, delimiters);
  stack_pointer = (void *) ((char **) stack_pointer - argc - 1);

  /*
   * push the token pointers onto the user stack, starting by pushing the first
   * token pointer to the bottom of the reserved space.
   * 
   * NOTE: the kernel thread operates in kernel virtual memory, and so sees the
   * token pointers as kernel virtual addresses as well. However, these addresses
   * will be directly referenced by the user process, so they must be converted
   * to user virtual addresses instead. Same rationale for converting argv into
   * user virtual address.
   */
  char *state_tracker, *token_pointer;
  token_pointer = strtok_r(user_argument, delimiters, &state_tracker);
  *((char **) stack_pointer) = (char *) ktou(token_pointer, *esp);
  for (
    int index = 1;
    (token_pointer = strtok_r(NULL, delimiters, &state_tracker)) != NULL && index <= argc;
    index++
  ) {
    *(((char **) stack_pointer) + index) = (char *) ktou(token_pointer, *esp);
  }
  *((char **) stack_pointer + argc) = NULL;
  char **argv = (char **) ktou(stack_pointer, *esp);

  /*
   * find the offset between stack_pointer and the closest stack-aligned address.
   * If the offset is less than the minimum needed to store the main() arguments,
   * add enough 4-words unit until it's large enough.
   * 
   * For some reason, the entry stub moves the stack pointer down by 28 bytes,
   * and expects the stack pointer to be stack-aligned afterwards. Therefore,
   * our stack pointer must be 4 bytes below a stack-aligned address here.
   */
  size_t alignment_offset = ((uint32_t) stack_pointer) % 16;
  size_t minimum_offset = sizeof(char**) + sizeof(int) + sizeof(void*);
  while ((alignment_offset + 4) < minimum_offset)
    alignment_offset += 16;
  stack_pointer = (void *) ((uint32_t) stack_pointer - (alignment_offset + 4));

  // push the char *argv[], int argc, and return address onto the stack
  *((char ***)((uint32_t) stack_pointer + sizeof(void*) + sizeof(int))) = argv;
  *((int *)((uint32_t) stack_pointer + sizeof(void*))) = argc;
  *((void **) stack_pointer) = NULL;

  // update the ESP
  uint32_t difference = (char*) utok(t->pagedir, *esp) - (char *) stack_pointer;
  *esp = (void *) ((uint32_t)(* esp) - difference);
  
  return true;
}

/* load() helpers. */
static int USER_STACK_OFFSET_IN_BYTES = 20;
static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file)
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file))
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz)
    return false;

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0)
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Get a page of memory. */
      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      /* Load this page. */
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false;
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      /* Add the page to the process's address space. */
      if (!install_page (upage, kpage, writable))
        {
          palloc_free_page (kpage);
          return false;
        }

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp)
{
  uint8_t *kpage;
  bool success = false;

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL)
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      /*
       * the %esp value assigned here will be restored into the ESP register as
       * intr-stubs.S:intr_exit executes the `ret` instruction, so it will be
       * the initial user stack pointer right after the context switch.
       * 
       * We need to reserve space for char *argv[], int argc, and the return
       * address, as well as to ensure that the %esp pointing to the return
       * address is stack-aligned (on 4-word). In addition, in case those
       * arguments to main() aren't pushed onto the stack, we also need to
       * prevent the user process from referencing kernel memory while trying
       * to access them at %esp+8, %esp+4, and %esp. Therefore, an offset of 20
       * bytes is pushed onto the stack.
       */
      if (success)
        *esp = PHYS_BASE - USER_STACK_OFFSET_IN_BYTES;
      else
        palloc_free_page (kpage);
    }
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}
