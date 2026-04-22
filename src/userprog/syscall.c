#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "lib/kernel/stdio.h"
#include "pagedir.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "devices/shutdown.h"

static void syscall_bad_access_exit(void);

static bool validate_user_uaddr(const void* uaddr);

static bool read_user_u8(const uint8_t* uaddr, uint8_t* out);

static bool copy_in(void* kdst, const void* usrc, size_t size);

static bool fetch_arg_u32(struct intr_frame* f, size_t index, uint32_t* out);

static bool fetch_args_u32(struct intr_frame* f, size_t cnt, uint32_t out[]);

static bool validate_string(const char* ustr);

static bool copy_in_string(char* kdst, const char* ustr, size_t max_len);

static void syscall_handler(struct intr_frame*);

void syscall_init(void) { intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall"); }

static void syscall_bad_access_exit(void) {
  set_process_exit_status(-1);
  process_exit();
  NOT_REACHED();
}

static bool validate_user_uaddr(const void* uaddr) {
  if (uaddr == NULL) {
    return false;
  }

  if (!is_user_vaddr(uaddr)) {
    return false;
  }

  struct thread* cur = thread_current();
  if (cur->pcb == NULL) {
    return false;
  }
  if (cur->pcb->pagedir == NULL) {
    return false;
  }

  if (pagedir_get_page(cur->pcb->pagedir, uaddr) == NULL) {
    return false;
  }

  return true;
}

static bool read_user_u8(const uint8_t* uaddr, uint8_t* out) {
  if (validate_user_uaddr(uaddr) == false) {
    return false;
  }

  struct thread* cur = thread_current();
  void* page = pagedir_get_page(cur->pcb->pagedir, uaddr);
  *out = *((uint8_t*)uaddr);

  return true;
}

static bool copy_in(void* kdst, const void* usrc, size_t size) {
  if (kdst == NULL) {
    return false;
  }

  if (size == 0) {
    return true;
  }

  for (size_t i = 0; i < size; i++) {
    uint8_t out_u8 = 0;
    if (read_user_u8(usrc + i, &out_u8) == false) {
      return false;
    }
    *((uint8_t*)(kdst + i)) = out_u8;
  }

  return true;
}

static bool fetch_arg_u32(struct intr_frame* f, size_t index, uint32_t* out) {
  if (f == NULL) {
    return false;
  }

  uint8_t* arg_addr = f->esp + index * 4;
  uint32_t out_u32 = 0;
  if (copy_in(&out_u32, arg_addr, 4) == false) {
    return false;
  }
  *out = out_u32;

  return true;
}

static bool fetch_args_u32(struct intr_frame* f, size_t cnt, uint32_t out[]) {
  if (f == NULL) {
    return false;
  }

  uint8_t* arg_addr = f->esp;
  for (size_t i = 0; i < cnt; i++) {
    if (fetch_arg_u32(f, i, out + i) == false) {
      return false;
    }
  }

  return true;
}

static bool validate_string(const char* ustr) {
  if (ustr == NULL) {
    return false;
  }

  for (size_t i = 0; i < 4096; i++) {
    char ch;
    if (read_user_u8((uint8_t*)ustr + i, (uint8_t*)&ch) == false) {
      return false;
    }
    if (ch == '\0') {
      return true;
    }
  }

  return false;
}

static bool copy_in_string(char* kdst, const char* ustr, size_t max_len) {
  if (kdst == NULL || ustr == NULL) {
    return false;
  }

  uint8_t out_u8 = 0;
  for (size_t i = 0; i < max_len; i++) {
    if (ustr[i] == '\0') {
      return true;
    }
    if (read_user_u8((uint8_t*)(ustr + i), &out_u8) == false) {
      return false;
    }
    kdst[i] = (char)out_u8;
  }

  if (ustr[max_len] == '\0') {
    kdst[max_len] = '\0';
    return true;
  }
  return false;
}

static void syscall_handler(struct intr_frame* f) {
  /* Note: Do NOT dereference arg[idx]! Check memory validity FIRST! */
  uint32_t* args = ((uint32_t*)f->esp);
  uint32_t arg0;
  if (fetch_arg_u32(f, 0, &arg0) == false) {
    syscall_bad_access_exit();
  }

  /*
   * The following print statement, if uncommented, will print out the syscall
   * number whenever a process enters a system call. You might find it useful
   * when debugging. It will cause tests to fail, however, so you should not
   * include it in your final submission.
   */

  /* printf("System call number: %d\n", arg0); */

  switch (arg0) {
    case SYS_WRITE: {
      int fd;
      if (fetch_arg_u32(f, 1, (uint32_t*)&fd) == false) {
        syscall_bad_access_exit();
      }
      const char* buffer;
      if (fetch_arg_u32(f, 2, (uint32_t*)&buffer) == false) {
        syscall_bad_access_exit();
      }
      unsigned size;
      if (fetch_arg_u32(f, 3, (uint32_t*)(&size)) == false) {
        syscall_bad_access_exit();
      }

      if (fd == STDOUT_FILENO) {
        putbuf(buffer, size);
        f->eax = (int)size;
      } else {
        f->eax = -1;
      }
      break;
    }

    case SYS_EXIT: {
      if (fetch_arg_u32(f, 1, &(f->eax)) == false) {
        syscall_bad_access_exit();
      }
      set_process_exit_status(f->eax);
      process_exit();
      break;
    }

    case SYS_PRACTICE: {
      int i;
      if (fetch_arg_u32(f, 1, (uint32_t*)&i) == false) {
        syscall_bad_access_exit();
      }
      f->eax = i + 1;
      set_process_exit_status(0);
      break;
    }

    case SYS_HALT: {
      shutdown_power_off();
      break;
    }

    case SYS_EXEC: {
      const char* cmd_line = NULL;
      if (fetch_arg_u32(f, 1, (uint32_t*)&cmd_line) == false) {
        syscall_bad_access_exit();
      }
      if (validate_string(cmd_line) == false) {
        syscall_bad_access_exit();
      }
      size_t len = strlen(cmd_line);
      char* buffer = malloc(len + 1);
      if (copy_in_string(buffer, cmd_line, len) == false) {
        syscall_bad_access_exit();
      }
      f->eax = process_execute(buffer);
      set_process_exit_status(f->eax);
      break;
    }

    case SYS_WAIT: {
      pid_t pid;
      if (fetch_arg_u32(f, 1, (uint32_t*)&pid) == false) {
        syscall_bad_access_exit();
      }
      f->eax = process_wait(pid);
      break;
    }
  }
}
