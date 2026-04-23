#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "list.h"
#include "pagedir.h"
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

// static struct semaphore temporary;
static thread_func start_process NO_RETURN;
static thread_func start_pthread NO_RETURN;
static bool load(const char* file_name, void (**eip)(void), void** esp, int argc, char** argv);
bool setup_thread(void (**eip)(void), void** esp);

struct child_sync {
  pid_t pid;                  /* Child proc's pid */
  int exit_status;            /* Child proc's exit status */
  bool exited;                /* Has the child proc exited */
  bool waited;                /* Has waited before */
  bool load_done;             /* Has the child proc finished loading */
  bool load_success;          /* Has the child proc successfully loaded */
  struct semaphore load_sema; /* Loading semaphore */
  struct semaphore exit_sema; /* Waiting semaphore */
  int ref_cnt;                /* Reference count */
  struct lock lock;           /* Lock for concurrency */
  struct list_elem elem;      /* List of children */
};

struct child_sync* child_sync_create(pid_t pid);

void child_sync_get(struct child_sync* cs);

void child_sync_put(struct child_sync* cs);

void child_sync_set_load(struct child_sync* cs, bool success);

void child_sync_set_exit(struct child_sync* cs, int status);

struct child_sync* child_sync_find(struct process* pcb, pid_t child_pid);

void process_release_children(struct process* pcb);

struct child_sync* child_sync_create(pid_t pid) {
  struct child_sync* cs = calloc(1, sizeof(struct child_sync));
  if (cs == NULL) {
    return NULL;
  }
  cs->pid = pid;
  cs->exit_status = -1;
  cs->ref_cnt = 1;
  lock_init(&cs->lock);
  sema_init(&cs->load_sema, 0);
  sema_init(&cs->exit_sema, 0);
  return cs;
}

void child_sync_get(struct child_sync* cs) {
  if (cs == NULL) {
    return;
  }
  lock_acquire(&cs->lock);
  cs->ref_cnt++;
  lock_release(&cs->lock);
}

void child_sync_put(struct child_sync* cs) {
  if (cs == NULL) {
    return;
  }
  bool need_free = false;
  lock_acquire(&cs->lock);
  cs->ref_cnt--;
  if (cs->ref_cnt == 0) {
    need_free = true;
  }
  lock_release(&cs->lock);
  if (need_free) {
    free(cs);
  }
}

void child_sync_set_load(struct child_sync* cs, bool success) {
  if (cs == NULL) {
    return;
  }
  lock_acquire(&cs->lock);
  cs->load_done = true;
  cs->load_success = success;
  lock_release(&cs->lock);
  sema_up(&cs->load_sema);
}

void child_sync_set_exit(struct child_sync* cs, int status) {
  if (cs == NULL) {
    return;
  }
  lock_acquire(&cs->lock);
  cs->exited = true;
  cs->exit_status = status;
  lock_release(&cs->lock);
  sema_up(&cs->exit_sema);
}

struct child_sync* child_sync_find(struct process* pcb, pid_t child_pid) {
  if (pcb == NULL) {
    return NULL;
  }
  for (struct list_elem* e = list_begin(&pcb->children); e != list_end(&pcb->children);
       e = list_next(e)) {
    struct child_sync* cs = list_entry(e, struct child_sync, elem);
    if (cs->pid == child_pid) {
      return cs;
    }
  }
  return NULL;
}

void process_release_children(struct process* pcb) {
  if (pcb == NULL) {
    return;
  }
  while (!list_empty(&pcb->children)) {
    struct child_sync* cs = list_entry(list_pop_front(&pcb->children), struct child_sync, elem);
    child_sync_put(cs);
  }
}

/* Initializes user programs in the system by ensuring the main
   thread has a minimal PCB so that it can execute and wait for
   the first user process. Any additions to the PCB should be also
   initialized here if main needs those members */
void userprog_init(void) {
  struct thread* t = thread_current();
  bool success;

  /* Allocate process control block
     It is imoprtant that this is a call to calloc and not malloc,
     so that t->pcb->pagedir is guaranteed to be NULL (the kernel's
     page directory) when t->pcb is assigned, because a timer interrupt
     can come at any time and activate our pagedir */
  t->pcb = calloc(sizeof(struct process), 1);
  success = t->pcb != NULL;

  /* Kill the kernel if we did not succeed */
  ASSERT(success);

  t->pcb->main_thread = t;
  t->pcb->exit_status = -1;
  t->pcb->self_sync = NULL;
  strlcpy(t->pcb->process_name, t->name, sizeof t->pcb->process_name);
  list_init(&t->pcb->children);
}

/* Argument structure for passing args */
struct process_args {
  int argc;
  char* argv[128];
  // char *cmdline;
};

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   process id, or TID_ERROR if the thread cannot be created. */
pid_t process_execute(const char* file_name) {
  char* fn_copy;
  tid_t tid;

  /* Alloc memory for storing arguments */
  struct process_args* args = malloc(sizeof(struct process_args));
  if (args == NULL) {
    return TID_ERROR;
  }

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page(0);
  if (fn_copy == NULL) {
    free(args);
    return TID_ERROR;
  }
  strlcpy(fn_copy, file_name, PGSIZE);

  /* Parse all args and reserve 2 slots: one for sync payload and one NULL. */
  args->argc = 0;
  char* save_ptr = NULL;
  for (char* token = strtok_r(fn_copy, " ", &save_ptr); token != NULL;
       token = strtok_r(NULL, " ", &save_ptr)) {
    if (args->argc >= 126) {
      palloc_free_page(fn_copy);
      free(args);
      return TID_ERROR;
    }
    args->argv[args->argc++] = token;
  }

  if (args->argc == 0) {
    palloc_free_page(fn_copy);
    free(args);
    return TID_ERROR;
  }

  struct child_sync* cs = child_sync_create(0);
  if (cs == NULL) {
    palloc_free_page(fn_copy);
    free(args);
    return TID_ERROR;
  }

  /* Parent list owns one ref, child thread owns one ref. */
  child_sync_get(cs);
  args->argv[args->argc++] = (char*)cs;
  args->argv[args->argc] = NULL;

  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create(args->argv[0], PRI_DEFAULT, start_process, args);
  if (tid == TID_ERROR) {
    child_sync_put(cs);
    child_sync_put(cs);
    palloc_free_page(fn_copy);
    free(args);
    return TID_ERROR;
  }

  cs->pid = tid;
  list_push_front(&thread_current()->pcb->children, &cs->elem);
  sema_down(&cs->load_sema);
  if (cs->load_success == false) {
    list_remove(&cs->elem);
    child_sync_put(cs);
    return TID_ERROR;
  }

  return tid;
}

struct fork_args {
  struct intr_frame parent_if;
  struct child_sync* cs;
  struct process* parent_pcb;
  char child_name[16];
};

static void start_fork(void* args_) {
  struct fork_args* args = (struct fork_args*)args_;
  struct child_sync* cs = args->cs;
  struct process* parent_pcb = args->parent_pcb;
  struct thread* t = thread_current();
  struct intr_frame if_;
  bool success = false;

  /* Allocate and initialize PCB for the child process */
  struct process* child_pcb = malloc(sizeof(struct process));
  if (child_pcb == NULL) {
    goto done;
  }

  child_pcb->pagedir = NULL;
  child_pcb->main_thread = t;
  child_pcb->exit_status = -1;
  list_init(&child_pcb->children);
  child_pcb->self_sync = cs;
  strlcpy(child_pcb->process_name, args->child_name, sizeof(child_pcb->process_name));

  t->pcb = child_pcb;

  /* Create child page directory and clone parent user memory */
  child_pcb->pagedir = pagedir_create();
  if (child_pcb->pagedir == NULL) {
    goto done;
  }
  if (!clone_pagedir(child_pcb, parent_pcb)) {
    goto done;
  }

  /* Clone file discriptors */
  /* TBD */

  /* Prepare the child process's interrupt frame */
  memcpy(&if_, &args->parent_if, sizeof(if_));
  if_.eax = 0;

  process_activate();
  success = true;

done:
  /* Notice the parent process */
  child_sync_set_load(cs, success);
  free(args);

  if (!success) {
    /* Child failed before reaching userspace: drop the child-owned ref. */
    child_sync_put(cs);

    /* Clone failed, cleanup and quit */
    if (child_pcb != NULL) {
      if (child_pcb->pagedir != NULL) {
        pagedir_destroy(child_pcb->pagedir);
      }
      free(child_pcb);
    }
    t->pcb = NULL;
    thread_exit();
  }

  /* Jump to user state and execute */
  asm volatile("movl %0, %%esp; jmp intr_exit" : : "g"(&if_) : "memory");
  NOT_REACHED();
}

pid_t process_fork(struct intr_frame* parent_if) {
  struct thread* cur = thread_current();
  struct fork_args* args;
  struct child_sync* cs;
  tid_t tid;

  /* Create a sync struct for the child process */
  cs = child_sync_create(0);
  if (cs == NULL) {
    return TID_ERROR;
  }

  /* Allocate and initialize fork arguments */
  args = malloc(sizeof(struct fork_args));
  if (args == NULL) {
    child_sync_put(cs);
    return TID_ERROR;
  }

  /* Copy the parent process's interrupt frame */
  memcpy(&args->parent_if, parent_if, sizeof(struct intr_frame));
  args->cs = cs;
  args->parent_pcb = cur->pcb;

  /* Child keeps the same process name as the parent process. */
  strlcpy(args->child_name, cur->pcb->process_name, sizeof(args->child_name));

  /* Reserve a child sync reference for the child thread */
  child_sync_get(cs);

  /* Create the kernel thread to execute start_fork */
  tid = thread_create(args->child_name, PRI_DEFAULT, start_fork, args);
  if (tid == TID_ERROR) {
    /* Release the child and parent references of sync */
    child_sync_put(cs);
    child_sync_put(cs);
    free(args);
    return TID_ERROR;
  }

  /* Setup child process's PID */
  cs->pid = tid;

  list_push_front(&cur->pcb->children, &cs->elem);

  sema_down(&cs->load_sema);
  if (!cs->load_success) {
    list_remove(&cs->elem);
    child_sync_put(cs);
    return TID_ERROR;
  }

  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void start_process(void* args_) {
  struct process_args* args = (struct process_args*)args_;
  char* file_name = (char*)args->argv[0];
  struct child_sync* cs = (struct child_sync*)args->argv[args->argc - 1];
  args->argc--;
  args->argv[args->argc] = NULL;
  struct thread* t = thread_current();
  struct intr_frame if_;
  bool success;

  /* Allocate process control block */
  struct process* new_pcb = malloc(sizeof(struct process));
  success = new_pcb != NULL;

  /* Initialize process control block */
  if (success) {
    // Ensure that timer_interrupt() -> schedule() -> process_activate()
    // does not try to activate our uninitialized pagedir
    new_pcb->pagedir = NULL;
    new_pcb->main_thread = thread_current();
    new_pcb->exit_status = -1;
    list_init(&new_pcb->children);
    new_pcb->self_sync = cs;

    t->pcb = new_pcb;

    // Continue initializing the PCB as normal
    t->pcb->main_thread = t;
    strlcpy(t->pcb->process_name, t->name, sizeof t->name);
  }

  /* Initialize interrupt frame and load executable. */
  if (success) {
    memset(&if_, 0, sizeof if_);
    if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
    if_.cs = SEL_UCSEG;
    if_.eflags = FLAG_IF | FLAG_MBS;
    success = load(file_name, &if_.eip, &if_.esp, args->argc, args->argv);
  }

  /* Always wake parent waiting in process_execute(). */
  child_sync_set_load(cs, success);

  /* Clean up. Exit on failure or jump to userspace */
  palloc_free_page(file_name);
  free(args);
  if (!success) {
    set_process_exit_status(-1);
    process_exit();
    // thread_exit();
  }

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile("movl %0, %%esp; jmp intr_exit" : : "g"(&if_) : "memory");
  NOT_REACHED();
}

/* Waits for process with PID child_pid to die and returns its exit status.
   If it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If child_pid is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given PID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int process_wait(pid_t child_pid) {
  struct thread* cur = thread_current();
  struct child_sync* cs = child_sync_find(cur->pcb, child_pid);
  if (cs == NULL) {
    return -1;
  }

  bool has_exited = false;
  lock_acquire(&cs->lock);
  if (cs->waited == true) {
    lock_release(&cs->lock);
    return -1;
  }
  cs->waited = true;
  has_exited = cs->exited;
  lock_release(&cs->lock);

  if (!has_exited) {
    sema_down(&cs->exit_sema);
  }

  int local_status = cs->exit_status;
  list_remove(&cs->elem);
  child_sync_put(cs);

  return local_status;
}

/* Free the current process's resources. */
void process_exit(void) {
  struct thread* cur = thread_current();
  uint32_t* pd;

  /* If this thread does not have a PCB, don't worry */
  if (cur->pcb == NULL) {
    thread_exit();
    NOT_REACHED();
  }

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pcb->pagedir;
  if (pd != NULL) {
    /* Correct ordering here is crucial.  We must set
         cur->pcb->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
    cur->pcb->pagedir = NULL;
    pagedir_activate(NULL);
    pagedir_destroy(pd);
  }

  /* Output the process's exit status */
  printf("%s: exit(%d)\n", cur->pcb->process_name, cur->pcb->exit_status);

  if (cur->pcb->self_sync != NULL) {
    child_sync_set_exit(cur->pcb->self_sync, cur->pcb->exit_status);
    child_sync_put(cur->pcb->self_sync);
    cur->pcb->self_sync = NULL;
  }

  process_release_children(cur->pcb);

  /* Free the PCB of this process and kill this thread
     Avoid race where PCB is freed before t->pcb is set to NULL
     If this happens, then an unfortuantely timed timer interrupt
     can try to activate the pagedir, but it is now freed memory */
  struct process* pcb_to_free = cur->pcb;
  cur->pcb = NULL;
  free(pcb_to_free);

  thread_exit();
}

/* Sets the current process's exit status */
void set_process_exit_status(int status) {
  struct thread* cur = thread_current();

  if (cur->pcb == NULL) {
    return;
  }

  cur->pcb->exit_status = status;
}

/* Sets up the CPU for running user code in the current
   thread. This function is called on every context switch. */
void process_activate(void) {
  struct thread* t = thread_current();

  /* Activate thread's page tables. */
  if (t->pcb != NULL && t->pcb->pagedir != NULL)
    pagedir_activate(t->pcb->pagedir);
  else
    pagedir_activate(NULL);

  /* Set thread's kernel stack for use in processing interrupts.
     This does nothing if this is not a user process. */
  tss_update();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32 /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32 /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32 /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16 /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr {
  unsigned char e_ident[16];
  Elf32_Half e_type;
  Elf32_Half e_machine;
  Elf32_Word e_version;
  Elf32_Addr e_entry;
  Elf32_Off e_phoff;
  Elf32_Off e_shoff;
  Elf32_Word e_flags;
  Elf32_Half e_ehsize;
  Elf32_Half e_phentsize;
  Elf32_Half e_phnum;
  Elf32_Half e_shentsize;
  Elf32_Half e_shnum;
  Elf32_Half e_shstrndx;
};

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr {
  Elf32_Word p_type;
  Elf32_Off p_offset;
  Elf32_Addr p_vaddr;
  Elf32_Addr p_paddr;
  Elf32_Word p_filesz;
  Elf32_Word p_memsz;
  Elf32_Word p_flags;
  Elf32_Word p_align;
};

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL 0           /* Ignore. */
#define PT_LOAD 1           /* Loadable segment. */
#define PT_DYNAMIC 2        /* Dynamic linking info. */
#define PT_INTERP 3         /* Name of dynamic loader. */
#define PT_NOTE 4           /* Auxiliary info. */
#define PT_SHLIB 5          /* Reserved. */
#define PT_PHDR 6           /* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

static bool setup_stack(void** esp, int argc, char** argv);
static bool validate_segment(const struct Elf32_Phdr*, struct file*);
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool load(const char* file_name, void (**eip)(void), void** esp, int argc, char** argv) {
  struct thread* t = thread_current();
  struct Elf32_Ehdr ehdr;
  struct file* file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pcb->pagedir = pagedir_create();
  if (t->pcb->pagedir == NULL)
    goto done;
  process_activate();

  /* Open executable file. */
  file = filesys_open(file_name);
  if (file == NULL) {
    printf("load: %s: open failed\n", file_name);
    goto done;
  }

  /* Read and verify executable header. */
  if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr ||
      memcmp(ehdr.e_ident, "\177ELF\1\1\1", 7) || ehdr.e_type != 2 || ehdr.e_machine != 3 ||
      ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Elf32_Phdr) || ehdr.e_phnum > 1024) {
    printf("load: %s: error loading executable\n", file_name);
    goto done;
  }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) {
    struct Elf32_Phdr phdr;

    if (file_ofs < 0 || file_ofs > file_length(file))
      goto done;
    file_seek(file, file_ofs);

    if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
      goto done;
    file_ofs += sizeof phdr;
    switch (phdr.p_type) {
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
        if (validate_segment(&phdr, file)) {
          bool writable = (phdr.p_flags & PF_W) != 0;
          uint32_t file_page = phdr.p_offset & ~PGMASK;
          uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
          uint32_t page_offset = phdr.p_vaddr & PGMASK;
          uint32_t read_bytes, zero_bytes;
          if (phdr.p_filesz > 0) {
            /* Normal segment.
                     Read initial part from disk and zero the rest. */
            read_bytes = page_offset + phdr.p_filesz;
            zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
          } else {
            /* Entirely zero.
                     Don't read anything from disk. */
            read_bytes = 0;
            zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
          }
          if (!load_segment(file, file_page, (void*)mem_page, read_bytes, zero_bytes, writable))
            goto done;
        } else
          goto done;
        break;
    }
  }

  /* Set up stack. */
  if (!setup_stack(esp, argc, argv))
    goto done;

  /* Start address. */
  *eip = (void (*)(void))ehdr.e_entry;

  success = true;

done:
  /* We arrive here whether the load is successful or not. */
  file_close(file);
  return success;
}

/* load() helpers. */

static bool install_page(void* upage, void* kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool validate_segment(const struct Elf32_Phdr* phdr, struct file* file) {
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off)file_length(file))
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz)
    return false;

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr((void*)phdr->p_vaddr))
    return false;
  if (!is_user_vaddr((void*)(phdr->p_vaddr + phdr->p_memsz)))
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
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable) {
  ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT(pg_ofs(upage) == 0);
  ASSERT(ofs % PGSIZE == 0);

  file_seek(file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) {
    /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
    size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;

    /* Get a page of memory. */
    uint8_t* kpage = palloc_get_page(PAL_USER);
    if (kpage == NULL)
      return false;

    /* Load this page. */
    if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes) {
      palloc_free_page(kpage);
      return false;
    }
    memset(kpage + page_read_bytes, 0, page_zero_bytes);

    /* Add the page to the process's address space. */
    if (!install_page(upage, kpage, writable)) {
      palloc_free_page(kpage);
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
static bool setup_stack(void** esp, int argc, char** argv) {
  uint8_t* kpage;

  kpage = palloc_get_page(PAL_USER | PAL_ZERO);

  if (kpage == NULL) {
    return false;
  }

  if (!install_page(((uint8_t*)PHYS_BASE) - PGSIZE, kpage, true)) {
    palloc_free_page(kpage);
    return false;
  }

  uint32_t* stack_ptr = (uint32_t*)PHYS_BASE;
  char* argv_addrs[128];

  // Push argument strings into stack
  for (int i = argc - 1; i >= 0; i--) {
    size_t len = strlen(argv[i]) + 1;
    stack_ptr = (uint32_t*)((uint8_t*)stack_ptr - len);
    memcpy(stack_ptr, argv[i], len);
    argv_addrs[i] = (char*)stack_ptr;
  }

  // Align to 16-char boundary
  stack_ptr = (uint32_t*)((uint32_t)stack_ptr & ~0xf);

  // Push argv pointer array into stack
  *--stack_ptr = 0;
  for (int i = argc - 1; i >= 0; i--) {
    *--stack_ptr = (uint32_t)argv_addrs[i];
  }

  // Push argv pointer
  uint32_t argv_ptr = (uint32_t)stack_ptr;
  *--stack_ptr = argv_ptr;

  // Push argc
  *--stack_ptr = argc;

  *--stack_ptr = 0;

  // Set initial pointer
  *esp = stack_ptr;

  return true;
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
static bool install_page(void* upage, void* kpage, bool writable) {
  struct thread* t = thread_current();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page(t->pcb->pagedir, upage) == NULL &&
          pagedir_set_page(t->pcb->pagedir, upage, kpage, writable));
}

/* Returns true if t is the main thread of the process p */
bool is_main_thread(struct thread* t, struct process* p) { return p->main_thread == t; }

/* Gets the PID of a process */
pid_t get_pid(struct process* p) { return (pid_t)p->main_thread->tid; }

/* Creates a new stack for the thread and sets up its arguments.
   Stores the thread's entry point into *EIP and its initial stack
   pointer into *ESP. Handles all cleanup if unsuccessful. Returns
   true if successful, false otherwise.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. You may find it necessary to change the
   function signature. */
bool setup_thread(void (**eip)(void) UNUSED, void** esp UNUSED) { return false; }

/* Starts a new thread with a new user stack running SF, which takes
   TF and ARG as arguments on its user stack. This new thread may be
   scheduled (and may even exit) before pthread_execute () returns.
   Returns the new thread's TID or TID_ERROR if the thread cannot
   be created properly.

   This function will be implemented in Project 2: Multithreading and
   should be similar to process_execute (). For now, it does nothing.
   */
tid_t pthread_execute(stub_fun sf UNUSED, pthread_fun tf UNUSED, void* arg UNUSED) { return -1; }

/* A thread function that creates a new user thread and starts it
   running. Responsible for adding itself to the list of threads in
   the PCB.

   This function will be implemented in Project 2: Multithreading and
   should be similar to start_process (). For now, it does nothing. */
static void start_pthread(void* exec_ UNUSED) {}

/* Waits for thread with TID to die, if that thread was spawned
   in the same process and has not been waited on yet. Returns TID on
   success and returns TID_ERROR on failure immediately, without
   waiting.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
tid_t pthread_join(tid_t tid UNUSED) { return -1; }

/* Free the current thread's resources. Most resources will
   be freed on thread_exit(), so all we have to do is deallocate the
   thread's userspace stack. Wake any waiters on this thread.

   The main thread should not use this function. See
   pthread_exit_main() below.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
void pthread_exit(void) {}

/* Only to be used when the main thread explicitly calls pthread_exit.
   The main thread should wait on all threads in the process to
   terminate properly, before exiting itself. When it exits itself, it
   must terminate the process in addition to all necessary duties in
   pthread_exit.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
void pthread_exit_main(void) {}
