#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "filesys/directory.h"
#include "filesys/inode.h"
#include "threads/synch.h"
#include "devices/input.h"
#include "devices/block.h"
#include "userprog/process.h"

static void syscall_handler (struct intr_frame *);

typedef int pid_t;

// Process System Calls 
void halt (void) NO_RETURN;
void exit (int status) NO_RETURN;
pid_t exec (const char *file);
int wait (pid_t);

// File System Calls
bool create (const char *file, unsigned initial_size);
bool remove (const char *file);
int open (const char *file);
int filesize (int fd);
int read (int fd, void *buffer, unsigned length);
int write (int fd, const void *buffer, unsigned length);
void seek (int fd, unsigned position);
unsigned tell (int fd);
void close (int fd);

void check_valid_address(void *address);  
struct file_elem * find_file_elem(int fd);
int alloc_fd(void);

bool chdir(const char *);
bool mkdir(const char *);
bool readdir(int, const char *);
bool isdir(int);
bool inumber(int);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init(&filesys_lock);
}


/* check if address is null pointer or not in user address space
   if so, call exit(-1) */
void check_valid_address(void *address)  
{
  struct thread *t = thread_current();
  if(!address || !is_user_vaddr(address) || pagedir_get_page (t->pagedir, address) == NULL) exit(-1);
  return;
}


static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  int nsyscall, ret;
  int *esp = (int *)f->esp;

  //check esp is valid>
  check_valid_address(esp);
  nsyscall = *esp;

  switch(nsyscall)
  {
    case SYS_HALT:
      halt();
      break;
    case SYS_EXIT:
      check_valid_address((int *)(esp+1));
      exit(*(int *)(esp+1));
      break;
    case SYS_EXEC:
      check_valid_address((char *)*(esp+1));
      ret = exec((char *)*(esp+1));
      break;
    case SYS_WAIT:
      check_valid_address((pid_t *)(esp+1));
      ret = wait(*(esp+1));
      break;	

    case SYS_CREATE:
      check_valid_address((char *)*(esp+1));
      check_valid_address((unsigned *)(esp+2));
      ret = create((char *)*(esp+1), *(esp+2));
      break;
    case SYS_REMOVE:
      check_valid_address((char *)*(esp+1));
      ret = remove((char *)*(esp+1));
      break;
    case SYS_OPEN:
      check_valid_address((char *)*(esp+1));
      ret = open((char *)*(esp+1));
      break;
    case SYS_FILESIZE:
      check_valid_address((int *)(esp+1));
      ret = filesize(*(esp+1));
      break;
    case SYS_READ:
      check_valid_address((int *)(esp+1));
      check_valid_address((char *)*(esp+2));
      check_valid_address((unsigned *)(esp+3));
      ret = read(*(esp+1), (char *)*(esp+2), *(esp+3));
      break;
    case SYS_WRITE:
      check_valid_address((int *)(esp+1));
      check_valid_address((char *)*(esp+2));
      check_valid_address((unsigned *)(esp+3));
      ret = write(*(esp+1), (char *)*(esp+2), *(esp+3));
      break;
    case SYS_SEEK:
      check_valid_address((int *)(esp+1));
      check_valid_address((unsigned *)(esp+2));
      seek(*(esp+1), *(esp+2));
      break;
    case SYS_TELL:
      check_valid_address((int *)(esp+1));
      ret = tell(*(esp+1));
      break;
    case SYS_CLOSE:
      check_valid_address((int *)(esp+1));
      close(*(esp+1));
      break;
    case SYS_CHDIR:
      check_valid_address((char *)*(esp+1));
      ret = chdir((char *)*(esp+1));
      break;
    case SYS_MKDIR:
      check_valid_address((char *)*(esp+1));
      ret = mkdir((char *)*(esp+1));
      break;
    case SYS_READDIR:
      check_valid_address((int *)(esp+1));
      check_valid_address((char *)*(esp+2));
      ret = readdir((int *)(esp+1), (char *)*(esp+2));
      break;
    case SYS_ISDIR:
      check_valid_address((int *)(esp+1));
      ret = isdir((int *)(esp+1));
      break;
    case SYS_INUMBER:
      check_valid_address((int *)(esp+1));
      ret = inumber((int *)(esp+1));
      break;
    default:
      exit(-1);
      break;
  }  
  f->eax = ret;
}

/**** Process System Calls ****/

void halt (void)
{
  //printf("userprog/syscall.c	halt\n");  
  shutdown_power_off();
}

void exit (int status)
{
  //printf("userprog/syscall.c	exit\n");  
  struct thread *t = thread_current();
  t->exit_status = status;
  printf ("%s: exit(%d)\n",t->name,status);
  thread_exit ();
}

pid_t exec (const char *file)
{
  //printf("userprog/syscall.c	exec\n");  
  pid_t pid;
  check_valid_address(file);
  lock_acquire (&filesys_lock);  /* Do not allow file IO until process is loaded */
  pid = process_execute (file);
  lock_release (&filesys_lock);
  return pid;
}

int wait (pid_t pid)
{
  //printf("userprog/syscall.c	wait\n");  
  return process_wait (pid);
}



/**** File System Calls ****/

/* write system call,
   returns the number of bytes which are actually written */
int write (int fd, const void *buffer, unsigned length)
{
  int written = 0;
  if(fd == 0) exit(-1);// write to input (error)
  else if(fd == 1)  // write to console
  {
    lock_acquire(&filesys_lock);
    if(length < 512)
    {
      putbuf((char *)buffer, length);
      written = length;
    } else
    {
      while(length>512)
      {
        putbuf((char *)(buffer+written), 512);
	length -= 512;
	written += 512;
      }
      putbuf((char *)(buffer+written), length);
      written += length;
    }
    lock_release(&filesys_lock);
  } else  // write to a file
  {
    struct file_elem *fe = find_file_elem(fd);
    if(fe==NULL) exit(-1);
    else
    {
      struct file *f = fe->file;
      lock_acquire(&filesys_lock);
      written = file_write(f, buffer, length);
      lock_release(&filesys_lock);
    }
  }

  return written;
}


/* find a file_elem by fd */
struct file_elem * find_file_elem(int fd)
{
  struct list_elem *e;
  struct thread *t = thread_current();

  for(e = list_begin (&t->files); e != list_end (&t->files); e = list_next (e))
  {
    struct file_elem *fe = list_entry (e, struct file_elem, thread_elem);
    if (fe->fd == fd) return fe;
  }
  return NULL;
}


/* create system call, if succeeds, returns true */
bool create (const char *file, unsigned initial_size)
{
  bool ret;
  if(!file) exit(-1);
  else
  {
    lock_acquire(&filesys_lock);
    ret = filesys_create(file, initial_size, false);
    lock_release(&filesys_lock);
  }
  return ret;
}


/* remove system call, if succeeds, returns true */
bool remove (const char *file)
{
  bool ret;
  if(!file) exit(-1);
  else
  {
    lock_acquire(&filesys_lock);
    ret = filesys_remove(file);
    lock_release(&filesys_lock);
  }
  return ret;
}


/* open system call
  if succeeds returns its file descriptor. if not, returns -1 */
int open (const char *file)
{
  //printf("hi!\n");
  struct file *f;
  struct file_elem *fe;

  if(!file || strlen(file) == 0 ) return -1; // input name is null or empty

  lock_acquire(&filesys_lock);
  f = filesys_open(file);
  lock_release(&filesys_lock);

  if(!f) return -1;

  fe = (struct file_elem *)malloc(sizeof(struct file_elem));

  if(!fe) // fail to allocate memory
  {
    file_close(f);
    return -1; 
  }

  lock_acquire(&filesys_lock);

  if(inode_is_dir(file_get_inode(f)))
  {
    fe->dir = (struct dir *)f;
    fe->isdir = true;
    fe->fd = alloc_fd();
    list_push_back(&thread_current()->files, &fe->thread_elem);
  }
  else
  {
    fe->file = f;
    fe->isdir = false;
    fe->fd = alloc_fd();
    list_push_back(&thread_current()->files, &fe->thread_elem);
  }
  lock_release(&filesys_lock);

  return fe->fd;
}


/* returns allocated fd value */
int alloc_fd(void)
{
  static int fd = 2;
  return fd++;
}

/* returns file size */
int filesize (int fd)
{
  struct file_elem *fe = find_file_elem(fd);
  if(!fe) exit(-1);
  return file_length(fe->file);
}


/* returns the number of bytes actually read 
   returns -1 if it could not be read */
int read (int fd, void *buffer, unsigned length)
{
  int ret = 0;
  struct file_elem *fe;
  unsigned i;

  if(!is_user_vaddr(buffer)||(!is_user_vaddr(buffer+length))) return -1; // buffer is not in user virtual address
  
  if(fd == 0)  //stdin
  {
    for(i=0; i<length; i++)
    {
      *(uint8_t *)(buffer + i) = input_getc();
    }
    ret = length;
  } else if(fd == 1) return -1; // stdout
  else
  {
    lock_acquire(&filesys_lock);
    fe = find_file_elem(fd);
    if(!fe)
    { 
      lock_release(&filesys_lock);
      return -1;
    }
    ret = file_read(fe->file, buffer, length);
    lock_release(&filesys_lock);
  }

  return ret;
}

/* seek system call */
void seek (int fd, unsigned position)
{
  struct file_elem *fe = find_file_elem(fd);
  if(!fe) exit(-1); // if the file could not be found, call exit(-1)
  struct file *f = fe->file;
  lock_acquire(&filesys_lock);
  file_seek(f, position);
  lock_release(&filesys_lock);
}


/* tell system call */
unsigned tell (int fd)
{ 
  unsigned ret;
  struct file_elem *fe = find_file_elem(fd);
  if(!fe) exit(-1); // if the file could not be found, call exit(-1)
  struct file *f = fe->file;
  lock_acquire(&filesys_lock);
  ret = file_tell(f);
  lock_release(&filesys_lock);

  return ret;
}


/* close system call */
void close (int fd)
{
  struct file_elem *fe = find_file_elem(fd);
  if(!fe) exit(-1); // if the file could not be found, call exit(-1)

  lock_acquire(&filesys_lock);
  if(fe->isdir)
  {
    dir_close(fe->dir);
  }
  else
  {
    file_close(fe->file);
  }

  list_remove(&fe->thread_elem);
  lock_release(&filesys_lock);

  free(fe);
}

bool chdir(const char *dir)
{
  return filesys_chdir(dir);
}

bool mkdir(const char *dir)
{
  if(strcmp(dir, "") == 0) return false;
  return filesys_create(dir, 0, true);
}

bool readdir(int fd, const char *name)
{
  struct file_elem *fe = find_file_elem(fd);

  if(!fe) return false;
  if(!fe->isdir) return false;
  if(!dir_readdir(fe->dir, name)) return false;

  return true;
}

bool isdir(int fd)
{
  struct file_elem *fe = find_file_elem(fd);

  if(!fe) return false;
  return fe->isdir;
}

bool inumber(int fd)
{
  block_sector_t inumber;
  struct file_elem *fe = find_file_elem(fd);

  if(!fe) return false;
  if(fe->isdir) inumber = inode_get_inumber(dir_get_inode(fe->dir));
  else inumber = inode_get_inumber(file_get_inode(fe->file));

  return inumber;
}
