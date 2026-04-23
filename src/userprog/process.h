#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "threads/interrupt.h"
#include <stdint.h>

// At most 8MB can be allocated to the stack
// These defines will be used in Project 2: Multithreading
#define MAX_STACK_PAGES (1 << 11)
#define MAX_THREADS 127

/* PIDs and TIDs are the same type. PID should be
   the TID of the main thread of the process */
typedef tid_t pid_t;

/* Thread functions (Project 2: Multithreading) */
typedef void (*pthread_fun)(void*);
typedef void (*stub_fun)(pthread_fun, void*);

/* The process control block for a given process. Since
   there can be multiple threads per process, we need a separate
   PCB from the TCB. All TCBs in a process will have a pointer
   to the PCB, and the PCB will have a pointer to the main thread
   of the process, which is `special`. */
struct process {
  /* Owned by process.c. */
  uint32_t* pagedir;            /* Page directory. */
  char process_name[16];        /* Name of the main thread */
  struct thread* main_thread;   /* Pointer to main thread */
  int exit_status;              /* Status exit code */
  struct list children;         /* List of child processes */
  struct child_sync* self_sync; /* Pointer to sync struct */
  struct list fd_list;          /* List of file discriptors*/
  int next_fd;                  /* Next fd to alloc(start from 2)*/
  struct file* exec_file;       /* Current running executable*/
};

struct file_handle {
  struct file* file; /* Pointer to file */
  int ref_cnt;       /* Reference count */
  struct lock lock;  /* Lock for concurrency */
};

struct fd_entry {
  int fd;                     /* File discriptor*/
  struct file_handle* handle; /* File handle pointer */
  struct list_elem elem;      /* List of fd entries */
};

void userprog_init(void);

pid_t process_execute(const char* file_name);
pid_t process_fork(struct intr_frame* parent_if);
int process_wait(pid_t);
void process_exit(void);
void process_activate(void);
void set_process_exit_status(int status);

bool is_main_thread(struct thread*, struct process*);
pid_t get_pid(struct process*);

tid_t pthread_execute(stub_fun, pthread_fun, void*);
tid_t pthread_join(tid_t);
void pthread_exit(void);
void pthread_exit_main(void);

struct file_handle* file_handle_create(struct file* file);
void file_handle_get(struct file_handle* handle);
void file_handle_put(struct file_handle* handle);

struct fd_entry* fd_lookup(struct process* pcb, int fd);
int fd_install(struct process* pcb, struct file* file);
void fd_close(struct process* pcb, int fd);
void fd_close_all(struct process* pcb);

#endif /* userprog/process.h */
