#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

static struct semaphore temporary;
static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name)
{
  char *fn_copy;
  tid_t tid;

  sema_init (&temporary, 0);
  /* 
   * Allocate a kernel page for the new process, and copy the file_name from the
   * parent process's kernel memory into the child process's kernel memory;
   * otherwise there's a race between the caller and load(). This kernel page
   * is not put into the parent process page table.
   */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);

  // Create a new kernel thread at default priority to execute FILE_NAME.
  tid = thread_create (file_name, PRI_DEFAULT, start_process, fn_copy);
  if (tid == TID_ERROR)
    palloc_free_page (fn_copy);
  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *file_name_)
{
  /*
   * We are now in the child process's kernel thread. The interrupt frame if_
   * is on the child process kernel page (see thread.c:thread_create()), but
   * file_name is in a temporary kernel page created by the parent process,
   * so it must freed before the child process starts.
   */
  char *file_name = file_name_;
  struct intr_frame if_;
  bool success;

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load (file_name, &if_.eip, &if_.esp);
  
  /*
   * Free the temporary kernel page now that the program call arguments in
   * file_name are copied into user memory
   */
  palloc_free_page (file_name);

  // If either load failed, quit.
  if (!success)
    thread_exit ();

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

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (tid_t child_tid UNUSED)
{
  sema_down (&temporary);
  return 0;
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL)
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
  sema_up (&temporary);
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

  file = filesys_open (program_name);
  if (file == NULL) {
    printf ("load: %s: open failed\n", program_name);
    goto done;
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
  /* We arrive here whether the load is successful or not. */
  file_close (file);
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
  void *stack_pointer = utok(*esp);

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
  if (argument_length == max_argument_length)
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
   */
  size_t alignment_offset = ((uint32_t) stack_pointer) % 16;
  size_t minimum_offset = sizeof(char**) + sizeof(int) + sizeof(void*);
  while (alignment_offset < minimum_offset)
    alignment_offset += sizeof(void *) * 4;
  stack_pointer = (void *) ((uint32_t) stack_pointer - alignment_offset);

  // push the char *argv[], int argc, and return address onto the stack
  *((char ***)((uint32_t) stack_pointer + sizeof(void*) + sizeof(int))) = argv;
  *((int *)((uint32_t) stack_pointer + sizeof(void*))) = argc;
  *((void **) stack_pointer) = NULL;

  // update the ESP
  int difference = (char*) pagedir_get_page(t->pagedir, *esp) - (char *) stack_pointer;
  *esp = (void *) (*((char **) esp) - difference);
  
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
