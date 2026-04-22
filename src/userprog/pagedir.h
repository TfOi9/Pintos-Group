#ifndef USERPROG_PAGEDIR_H
#define USERPROG_PAGEDIR_H

#include <stdbool.h>
#include <stdint.h>
#include "process.h"

uint32_t* pagedir_create(void);
void pagedir_destroy(uint32_t* pd);
bool pagedir_set_page(uint32_t* pd, void* upage, void* kpage, bool rw);
void* pagedir_get_page(uint32_t* pd, const void* upage);
void pagedir_clear_page(uint32_t* pd, void* upage);
bool pagedir_is_dirty(uint32_t* pd, const void* upage);
void pagedir_set_dirty(uint32_t* pd, const void* upage, bool dirty);
bool pagedir_is_accessed(uint32_t* pd, const void* upage);
void pagedir_set_accessed(uint32_t* pd, const void* upage, bool accessed);
void pagedir_activate(uint32_t* pd);
uint32_t* active_pd(void);

bool clone_pagedir(struct process* child_pcb, struct process* parent_pcb);
bool clone_user_page(uint32_t* child_pd, uint32_t* parent_pd, void* upage);
bool is_user_page_writable(uint32_t* pd, const void* upage);

#endif /* userprog/pagedir.h */
