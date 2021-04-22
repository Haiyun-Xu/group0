#include "descriptor.h"

/*
 * Initialize the original Pintos process's file structures page.
 */
void descriptor_init(void) {
  if (!setup_file_descriptors())
    PANIC("descriptor_init: failed to create process files page");
  
  lock_init(&filesys_lock);
}

/*
 * Set up the process's file structures page.
 */
bool setup_file_descriptors(void) {
  void *file_descriptor_page = palloc_get_page(PAL_ZERO);
  if (file_descriptor_page == NULL)
    return false;
  
  thread_current()->file_descriptor_page = file_descriptor_page;
  return true;
}

/*
 * Returns a new and initialized file descriptor structure. If there's no more
 * space available, returns a null pointer.
 */
static struct descriptor *get_new_file_descriptor(void) {
  struct thread *t = thread_current();
  struct descriptor *descriptor_list = t->file_descriptor_page;
  int capacity = PGSIZE / sizeof(struct descriptor);

  // iterate through the page, return the first file descriptor available
  for (int index = 0; index <= capacity; index++) {
    if (!descriptor_list[index].in_use) {
      descriptor_list[index].in_use = true;
      // file descriptors start at 2, becaose STDIN_FILENO/STDOUT_FILENO are 0/1
      descriptor_list[index].fd = 2 + index;
      return &descriptor_list[index];
    }
  }

  return NULL;
}

/*
 * Returns the file descriptor structure associated with the descriptor fd.
 * If fd is not a valid descriptor owned by the calling process, returns a null
 * pointer.
 */
static struct descriptor *get_file_descriptor(int fd) {
  // descriptors less than 2 do not have corresponding file descriptors
  if (fd <= 1)
    return NULL;
  
  struct thread *t = thread_current();
  struct descriptor *descriptor_list = t->file_descriptor_page;
  int capacity = PGSIZE / sizeof(struct descriptor);
  
  for (int index = 0; index <= capacity; index++) {
    if (descriptor_list[index].in_use && descriptor_list[index].fd == 2 + index) {
      return &descriptor_list[index];
    }
  }

  return NULL;
}

/*
 * Open a process's own executable, prohibiting write on it, and return the
 * file structure.
 */
struct file *open_process_executable(const char* file_name) {
  lock_acquire(&filesys_lock);

  struct file *program = NULL;
  program = filesys_open(file_name);
  if (program != NULL)
    file_deny_write(program);

  lock_release(&filesys_lock);
  return program;
}

/*
 * Close a process's own executable, allowing write to it.
 */
void close_process_executable(void) {
  lock_acquire(&filesys_lock);

  struct thread *t = thread_current();
  file_close(t->process_executable);
  
  lock_release(&filesys_lock);
}

/*
 * Closes all the file descriptors of the current process. Used when the process
 * is exiting.
 */
void close_all_file_descriptors(void) {
  lock_acquire(&filesys_lock);

  struct thread *t = thread_current();
  struct descriptor *descriptor_list = t->file_descriptor_page;
  int capacity = PGSIZE / sizeof(struct descriptor);
  
  for (int index = 0; index <= capacity; index++) {
    if (descriptor_list[index].in_use) {
      descriptor_list[index].in_use = false;
      file_close(descriptor_list[index].file_ptr);
    }
  }
  memset(t->file_descriptor_page, 0, PGSIZE);

  lock_release(&filesys_lock);
}

bool fd_create (const char *file, unsigned initial_size) {
  lock_acquire(&filesys_lock);

  bool result = filesys_create(file, (off_t)initial_size);

  lock_release(&filesys_lock);
  
  return result;
}

/*
 * Removes and frees the given file's directory entry, but does not remove the
 * file per se. The removal operates on the file's directory inode, not on the
 * file's inode. This makes the removal of the file independent from the
 * closing/reads/writes of the file, and means that even after the removal, the
 * file can still be closed/read/written; it just can't be opened again, because
 * it's been removed from the directory and can't be found.
 */
bool fd_remove (const char *file) {
  lock_acquire(&filesys_lock);
  
  /*
   * does not close any file structures related to file, not even the ones in
   * the current process
   */
  bool result = filesys_remove(file);

  lock_release(&filesys_lock);

  return result;
}

/*
 * Opens the given file, and returns the file's fd, or -1 if it couldn't be opened.
 * Calling this function multiple times with the same file name returns different
 * fds, each of which must be closed independently.
 */
int fd_open (const char *file) {
  lock_acquire(&filesys_lock);

  int result = -1;
  struct descriptor *file_descriptor = get_new_file_descriptor();
  if (file_descriptor != NULL) {
    file_descriptor->file_ptr = filesys_open(file);
    /*
     * if the file cannot be opened, reset the allocated file descriptor;
     * otherwise, return the descriptor
     */
    if (file_descriptor->file_ptr == NULL) {
      memset(file_descriptor, 0, sizeof(struct descriptor));
    } else {
      result = file_descriptor->fd;
    }
  }

  lock_release(&filesys_lock);

  return result;
}

/**
 * Closes file descriptor fd. Exiting or terminating a process implicitly closes
 * all its open file descriptors, as if by calling this function for each one.
 */
void fd_close (int fd) {
  lock_acquire(&filesys_lock);

  struct descriptor *file_descriptor = get_file_descriptor(fd);
  if (file_descriptor != NULL) {
    // close the file structure, then free the file descriptor
    file_close(file_descriptor->file_ptr);
    memset(file_descriptor, 0, sizeof(struct descriptor));
  }

  lock_release(&filesys_lock);
}

/*
 * Read at most "size" bytes from fd into the buffer, and return the actual
 * number of bytes read (0 if at EOF, or -1 if fd could not be read). Assumes
 * that there's no need to end the buffer with a null-char.
 * 
 * If fd = STDIN, read from the keyboard using input_getc(). If STDIN is empty,
 * block and wait for inputs.
 */
int fd_read (int fd, void *buffer, unsigned size) {
  lock_acquire(&filesys_lock);

  /*
   * No null-char is appended to the end of the buffer, because the file might
   * not be text-based.
   */
  int result = -1;
  if (buffer == NULL || fd < 0 || fd == 1) {
    // edge case, skip to the end
  } else if (fd == 0) {
    /*
     * If fd = STDIN, read from the console. If the console input is longer than
     * the buffer, only "size" bytes are read; if the input is shorter than the
     * buffer and a newline-char is read, then we return the actual number of
     * bytes read.
     */
    char *buffer_char = (char *) buffer;
    unsigned index = 0;

    for (; index < size; index++) {
      buffer_char[index] = input_getc();
      if (buffer_char[index] == '\n')
        break;
    }

    result = index + 1;
  } else {
    // if fd >= 2, read from the file
    struct descriptor *file_descriptor = get_file_descriptor(fd);
    if (file_descriptor != NULL)
      result = (int) file_read(file_descriptor->file_ptr, buffer, (off_t) size);
  }

  lock_release(&filesys_lock);
  return result;
}

/*
 * Write at most "size" bytes from buffer into fd, and return the actual number
 * of bytes written. Writing past the EOF should extend the file, but since file
 * growth isn't implemented yet, the current behavior is to stop writing at the EOF.
 * 
 * If fd = STDOUT, write all of buffer to the console atomically using putbuf().
 */
int fd_write (int fd, const void *buffer, unsigned size) {
  lock_acquire(&filesys_lock);
  
  int result = 0;
  if (buffer == NULL || fd <= 0) {
    // edge case, skip to the end
  } else if (fd == 1) {
    // if fd == 1, write all of buffer to the console
    putbuf(buffer, (size_t) size);
    result = (int) size;
  } else {
    // if fd >= 2, write to the file
    struct descriptor *file_descriptor = get_file_descriptor(fd);
    if (file_descriptor != NULL)
      result = (int) file_write(file_descriptor->file_ptr, buffer, (off_t) size);
  }

  lock_release(&filesys_lock);
  return result;
}

/*
 * Changes the file position. If the position is past the current end of file,
 * a later read should return 0 bytes, and a later write should return an error.
 * 
 * Once Project 3 is complete, writes past the end of the file will simply extend
 * the file and fill any unwritten gap with zeros. Before that, it is assumed
 * that the existing filesystem implementation will implement the former semantics
 * and return an error.
 */
void fd_seek (int fd, unsigned position) {
  lock_acquire(&filesys_lock);
  
  struct descriptor *file_descriptor = get_file_descriptor(fd);
  if (file_descriptor != NULL)
    file_seek(file_descriptor->file_ptr, position);

  lock_release(&filesys_lock);
}

/*
 * Returns the position of the next byte in fd relative to the beginning of the
 * file. If fd is stdin/stdout or invalid, returns 0.
 */
unsigned fd_tell (int fd) {
  lock_acquire(&filesys_lock);
  
  unsigned result = 0;
  struct descriptor *file_descriptor = get_file_descriptor(fd);
  if (file_descriptor != NULL)
    result = file_tell(file_descriptor->file_ptr);

  lock_release(&filesys_lock);

  return result;
}

/*
 * Returns the size of the file fd. If fd is stdin/stdout or invalid, returns 0.
 */
int fd_filesize (int fd) {
  lock_acquire(&filesys_lock);
  
  int result = 0;
  struct descriptor *file_descriptor = get_file_descriptor(fd);
  if (file_descriptor != NULL)
    result = file_length(file_descriptor->file_ptr);
  
  lock_release(&filesys_lock);

  return result;
}
