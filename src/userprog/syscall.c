#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/pte.h"
#include "threads/thread.h"
#include "userprog/pagedir.h"

static void syscall_handler (struct intr_frame *);
static bool check_buffer (const void *, size_t, bool);
static bool check_string (const char *);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f)
{
  uint32_t number;
  uint32_t args[3];
  size_t argc;
  uintptr_t esp = (uintptr_t) f->esp;

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
      argc = 1;
      break;
    case SYS_READ:
    case SYS_WRITE:
      argc = 3;
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

  /* 4단계에서 각 API의 실제 반환값으로 대체한다. */
  f->eax = -1;
  switch (number)
    {
    case SYS_HALT:
      /* TODO: halt API 연결. 연결 전에는 사용자 코드로 복귀하지 않는다. */
      thread_exit ();
    case SYS_EXIT:
      /* TODO: exit((int) args[0]) 및 부모에게 종료 상태 전달. */
      thread_exit ();
    case SYS_EXEC:
      if (!check_string ((const char *) args[0]))
        goto invalid;
      /* TODO: 명령행을 커널 버퍼로 복사하고 exec 결과를 eax에 저장. */
      break;
    case SYS_WAIT:
      /* TODO: wait((tid_t) args[0]) 결과를 eax에 저장. */
      break;
    case SYS_READ:
    case SYS_WRITE:
      if (!check_buffer ((const void *) args[1], args[2],
                         number == SYS_READ))
        goto invalid;
      /* TODO: read/write((int) args[0], buffer, size) 결과를 eax에 저장. */
      break;
    }
  return;

 invalid:
  /* TODO: 4단계의 공통 exit(-1) 경로로 연결한다. */
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
