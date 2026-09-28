#include "userprog/syscall.h"
#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <syscall-nr.h>
#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/pte.h"
#include "threads/thread.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"

static void syscall_handler (struct intr_frame *);
static bool check_buffer (const void *, size_t, bool);
static bool check_string (const char *);
static void syscall_exit (int status) NO_RETURN;

struct fd_entry
  {
    int fd;
    struct file *file;
    struct list_elem elem;
  };

static struct fd_entry *find_fd (int fd);
static void close_file (struct fd_entry *entry);

void
syscall_init (void) 
{
  process_init ();
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f)
{
  uint32_t number;
  uint32_t args[4];
  size_t argc;
  uintptr_t esp = (uintptr_t) f->esp;
  struct thread *cur = thread_current ();
  struct fd_entry *entry;

  if (!check_buffer (f->esp, sizeof number, false))
    goto invalid;
  memcpy (&number, f->esp, sizeof number);

  switch (number)
    {
    case SYS_HALT:
      argc = 0;
      break;
    case SYS_EXIT:
    case SYS_EXEC:
    case SYS_WAIT:
    case SYS_FIBONACCI:
    case SYS_REMOVE:
    case SYS_OPEN:
    case SYS_CLOSE:
    case SYS_FILESIZE:
    case SYS_TELL:
      argc = 1;
      break;
    case SYS_CREATE:
    case SYS_SEEK:
      argc = 2;
      break;
    case SYS_READ:
    case SYS_WRITE:
      argc = 3;
      break;
    case SYS_MAX_OF_FOUR_INT:
      argc = 4;
      break;
    default:
      goto invalid;
    }

  if (argc > 0)
    {
      const void *user_args = (const void *) (esp + sizeof number);
      if (!check_buffer (user_args, argc * sizeof *args, false))
        goto invalid;
      memcpy (args, user_args, argc * sizeof *args);
    }

  switch (number)
    {
    case SYS_CREATE:
    case SYS_REMOVE:
    case SYS_OPEN:
      {
        const char *user_name = (const char *) args[0];
        char name[NAME_MAX + 1];

        if (!check_string (user_name))
          goto invalid;
        f->eax = number == SYS_OPEN ? -1 : 0;
        if (strlen (user_name) > NAME_MAX)
          break;
        strlcpy (name, user_name, sizeof name);
        if (number == SYS_OPEN)
          {
            if (cur->next_fd == INT_MAX)
              break;
            entry = malloc (sizeof *entry);
            if (entry == NULL)
              break;
            lock_acquire (&filesys_lock);
            entry->file = filesys_open (name);
            lock_release (&filesys_lock);
            if (entry->file == NULL)
              {
                free (entry);
                break;
              }
            entry->fd = cur->next_fd++;
            list_push_back (&cur->open_files, &entry->elem);
            f->eax = entry->fd;
          }
        else
          {
            if (number == SYS_CREATE && args[1] > INT_MAX)
              break;
            lock_acquire (&filesys_lock);
            f->eax = number == SYS_CREATE
                     ? filesys_create (name, args[1]) : filesys_remove (name);
            lock_release (&filesys_lock);
          }
        break;
      }
    case SYS_CLOSE:
      entry = find_fd ((int) args[0]);
      if (entry != NULL)
        close_file (entry);
      break;
    case SYS_FILESIZE:
    case SYS_SEEK:
    case SYS_TELL:
      entry = find_fd ((int) args[0]);
      f->eax = -1;
      if (entry == NULL)
        break;
      if (number == SYS_SEEK && args[1] > INT_MAX)
        goto invalid;
      lock_acquire (&filesys_lock);
      if (number == SYS_FILESIZE)
        f->eax = file_length (entry->file);
      else if (number == SYS_TELL)
        f->eax = file_tell (entry->file);
      else
        file_seek (entry->file, args[1]);
      lock_release (&filesys_lock);
      break;
    case SYS_FIBONACCI:
      f->eax = fibonacci ((int) args[0]);
      break;
    case SYS_MAX_OF_FOUR_INT:
      f->eax = max_of_four_int ((int) args[0], (int) args[1],
                                (int) args[2], (int) args[3]);
      break;
    case SYS_HALT:
      shutdown_power_off ();
    case SYS_EXIT:
      syscall_exit ((int) args[0]);
    case SYS_EXEC:
      if (!check_string ((const char *) args[0]))
        goto invalid;
      /* process_execute가 자식 생성 전에 명령행을 커널 페이지로 복사한다. */
      f->eax = process_execute ((const char *) args[0]);
      break;
    case SYS_WAIT:
      f->eax = process_wait ((tid_t) args[0]);
      break;
    case SYS_READ:
    case SYS_WRITE:
      if (!check_buffer ((const void *) args[1], args[2],
                         number == SYS_READ))
        goto invalid;
      if (number == SYS_READ && (int) args[0] == 0)
        {
          uint8_t *buffer = (uint8_t *) args[1];
          unsigned i;
          for (i = 0; i < args[2]; i++)
            buffer[i] = input_getc ();
          f->eax = args[2];
        }
      else if (number == SYS_WRITE && (int) args[0] == 1)
        {
          if (args[2] > 0)
            putbuf ((const char *) args[1], args[2]);
          f->eax = args[2];
        }
      else
        {
          entry = find_fd ((int) args[0]);
          f->eax = -1;
          if (entry == NULL || args[2] > INT_MAX)
            break;
          lock_acquire (&filesys_lock);
          f->eax = number == SYS_READ
                   ? file_read (entry->file, (void *) args[1], args[2])
                   : file_write (entry->file, (const void *) args[1], args[2]);
          lock_release (&filesys_lock);
        }
      break;
    }
  return;

 invalid:
  syscall_exit (-1);
}

/* FD 목록은 현재 프로세스만 접근하므로 FS lock 없이 조회한다. */
static struct fd_entry *
find_fd (int fd)
{
  struct list *files = &thread_current ()->open_files;
  struct list_elem *e;

  for (e = list_begin (files); e != list_end (files); e = list_next (e))
    {
      struct fd_entry *entry = list_entry (e, struct fd_entry, elem);
      if (entry->fd == fd)
        return entry;
    }
  return NULL;
}

/* 호출자는 FS lock을 보유하지 않는다. */
static void
close_file (struct fd_entry *entry)
{
  list_remove (&entry->elem);
  lock_acquire (&filesys_lock);
  file_close (entry->file);
  lock_release (&filesys_lock);
  free (entry);
}

/* 정상 종료와 사용자 예외 종료가 같은 경로에서 파일을 회수한다. */
void
syscall_close_files (void)
{
  struct list *files = &thread_current ()->open_files;

  while (!list_empty (files))
    close_file (list_entry (list_front (files), struct fd_entry, elem));
}

static void
syscall_exit (int status)
{
  thread_current ()->exit_status = status;
  thread_exit ();
}

/* 시작 주소만 검사하면 페이지 경계에 걸친 인자를 놓칠 수 있다. */
static bool
check_buffer (const void *buffer, size_t size, bool write)
{
  uint32_t *pd = thread_current ()->pagedir;
  uintptr_t addr = (uintptr_t) buffer;
  uint32_t flags = PTE_P | PTE_U | (write ? PTE_W : 0);

  if (size == 0)
    return true;
  if (buffer == NULL || !is_user_vaddr (buffer) || pd == NULL
      || size > (uintptr_t) PHYS_BASE - addr)
    return false;

  while (size > 0)
    {
      const void *page = (const void *) addr;
      uint32_t pde = pd[pd_no (page)];
      uint32_t *pt;
      size_t chunk = PGSIZE - pg_ofs (page);

      if ((pde & flags) != flags)
        return false;
      pt = pde_get_pt (pde);
      if ((pt[pt_no (page)] & flags) != flags
          || pagedir_get_page (pd, page) == NULL)
        return false;
      if (chunk >= size)
        return true;
      addr += chunk;
      size -= chunk;
    }
  return true;
}

static bool
check_string (const char *string)
{
  uintptr_t addr = (uintptr_t) string;

  while (check_buffer ((const void *) addr, 1, false))
    {
      const char *p = (const char *) addr;
      size_t chunk = PGSIZE - pg_ofs (p);
      size_t i;

      for (i = 0; i < chunk; i++)
        if (p[i] == '\0')
          return true;
      addr += chunk;
    }
  return false;
}

int
fibonacci (int n)
{
  unsigned prev = 0;
  unsigned next = 1;
  int i;

  for (i = 0; i < n; i++)
    {
      unsigned sum = prev + next;
      prev = next;
      next = sum;
    }
  return (int) prev;
}

int
max_of_four_int (int a, int b, int c, int d)
{
  int max = a;
  if (b > max)
    max = b;
  if (c > max)
    max = c;
  if (d > max)
    max = d;
  return max;
}
