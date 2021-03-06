#ifndef __SWAP_H
#define __SWAP_H

#include <stddef.h>
#include <stdbool.h>
#include <bitmap.h>
#include "threads/thread.h"

struct swap_page;

typedef size_t swap_t;
#define SWAP_FAIL ((swap_t) BITMAP_ERROR)

void swap_init (void);

size_t swap_stats_pages (void);
size_t swap_stats_full_pages (void);

// All functions must be called with interrupts enabled.
// swap_alloc_and_write may invoke process_dispose_unmodified_swap_page,
// the other functions won't.
// Length is in bytes, amount in pages.
// Most likely length is PGSIZE and amount is 1, resp.

bool swap_alloc_and_write (struct thread *owner,
                           void          *user_addr,
                           void          *src);
bool swap_read_and_retain (struct thread *owner,
                           void          *user_addr,
                           void          *dest);
bool swap_dispose (struct thread *owner,
                   void          *user_addr);
bool swap_must_retain (struct thread *owner,
                       void          *user_addr);

// May have interrupts enabled.

void swap_init_thread (struct thread *owner);
void swap_clean (struct thread *owner);

#endif
