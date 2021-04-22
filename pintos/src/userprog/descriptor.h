#ifndef DESCRIPTOR_H
#define DESCRIPTOR_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "devices/input.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "lib/debug.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

// a structure used to traslate file descriptor to "struct file *"
struct descriptor {
  bool in_use;
  int fd;
  struct file *file_ptr;
  char padding[0];
};

void descriptor_init(void);
bool setup_file_descriptors(void);
struct file *open_process_executable(const char* file_name);
void close_process_executable(void);
void close_all_file_descriptors(void);

bool fd_create (const char *file, unsigned initial_size);
bool fd_remove (const char *file);
int fd_open (const char *file);
int fd_filesize (int fd);
int fd_read (int fd, void *buffer, unsigned size);
int fd_write (int fd, const void *buffer, unsigned size);
void fd_seek (int fd, unsigned position);
unsigned fd_tell (int fd);
void fd_close (int fd);

#endif /* DESCRIPTOR_H */