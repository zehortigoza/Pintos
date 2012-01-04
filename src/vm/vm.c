#include "vm.h"
#include <hash.h>
#include <stddef.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "lru.h"
#include "swap.h"
#include "mmap.h"
#include "threads/vaddr.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "userprog/pagedir.h"

#define MIN_ALLOC_ADDR ((void *) (1<<16))
#define SWAP_PERCENT_AT_ONCE 5

static size_t swap_at_once;

#define MAX_ALLOC_RETRIES 32

#define VMLP_MAGIC (('V'<<16) + ('L'<<8) + 'P')
typedef char _CASSERT_VMLP_MAGIC24[0 - !(VMLP_MAGIC < (1<<24))];

enum vm_page_type
{
  VMPPT_UNUSED = 0,   // this physical page is not used, yet
  
  VMPPT_EMPTY,        // read from, but never written to -> all zeros
  VMPPT_USED,         // allocated, no swap file equivalent
  VMPPT_SWAPPED,      // retreived from swap and not dirty or disposed
                      // OR removed from RAM
  
  VMPPT_COUNT
};
typedef char _CASSERT_VMPPT_SIZE[0 - !(VMPPT_COUNT < (1<<7))];

struct vm_page
{
  void                *user_addr;   // virtual address
  struct thread       *thread;      // owner thread
  struct hash_elem     thread_elem; // for thread.vm_pages
  struct lru_elem      lru_elem;    // for pages_lru
  
  struct
  {
    uint32_t           vmlp_magic :24;
    bool               readonly   :1;
    enum vm_page_type  type       :7;
  };
};

static bool vm_is_initialized;
static struct lru pages_lru;
static struct lock vm_lock;

static void
assert_t_addr (struct thread *t, const void *addr)
{
  ASSERT (vm_is_initialized);
  ASSERT (t != NULL);
  ASSERT (addr >= MIN_ALLOC_ADDR);
  ASSERT (is_user_vaddr (addr));
  ASSERT (pg_ofs (addr) == 0);
}

void
vm_init (void)
{
  ASSERT (!vm_is_initialized);
  
  lru_init (&pages_lru, 0, NULL, NULL);
  lock_init (&vm_lock);
  
  size_t user_pool_size;
  palloc_fill_ratio (NULL, NULL, NULL, &user_pool_size);
  swap_at_once = (user_pool_size*SWAP_PERCENT_AT_ONCE + 99) / 100;
  if (swap_at_once < 3)
    swap_at_once = 3;
  
  vm_is_initialized = true;
  
  printf ("Initialized user's virtual memory. Swapping %u pages at once.\n",
          swap_at_once);
}

static inline struct vm_page *
vmlp_entry (const struct hash_elem *e, void *t)
{
  if (e == NULL)
    return NULL;
  struct vm_page *ee = hash_entry (e, struct vm_page, thread_elem);
  ASSERT (t == NULL || ee->thread == t);
  ASSERT (ee->vmlp_magic == VMLP_MAGIC);
  return ee;
}

static unsigned
vm_thread_page_hash (const struct hash_elem *e, void *t)
{
  typedef char _CASSERT[0 - !(sizeof (unsigned) == sizeof (void *))];
  
  ASSERT (t != NULL);
  return (unsigned) vmlp_entry (e,t)->user_addr;
}

static bool
vm_thread_page_less (const struct hash_elem *a,
                     const struct hash_elem *b,
                     void *t)
{
  return vm_thread_page_hash (a,t) < vm_thread_page_hash (b,t);
}

void
vm_init_thread (struct thread *t)
{
  ASSERT (vm_is_initialized);
  ASSERT (t != NULL);
  
  //printf ("   INITIALISIERE VM FÜR %8p.\n", t);
  hash_init (&t->vm_pages, &vm_thread_page_hash, &vm_thread_page_less, t);
}

static void
vm_dispose_real (struct vm_page *ee)
{
  ASSERT (lock_held_by_current_thread (&vm_lock));
  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (ee != NULL);
  ASSERT (ee->vmlp_magic == VMLP_MAGIC);
  
  lru_dispose (&pages_lru, &ee->lru_elem, false);
  hash_delete (&ee->thread->vm_pages, &ee->thread_elem);
  
  switch (ee->type)
    {
    case VMPPT_USED:
    case VMPPT_EMPTY:
      break;
      
    case VMPPT_SWAPPED:
      (void) swap_dispose (ee->thread, ee->user_addr);
      break;
      
    default:
      PANIC ("ee->type == %d", ee->type);
    }
    
  void *kpage = pagedir_get_page (ee->thread->pagedir, ee->user_addr);
  if (kpage != NULL)
    {
      pagedir_clear_page (ee->thread->pagedir, ee->user_addr);
      palloc_free_page (kpage);
    }
  
  free (ee);
}

static void
vm_clean_sub (struct hash_elem *e, void *t)
{
  ASSERT (lock_held_by_current_thread (&vm_lock));
  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (e != NULL);
  
  struct vm_page *ee = vmlp_entry (e,t);
  
  vm_dispose_real (ee);
}

void
vm_clean (struct thread *t)
{
  ASSERT (vm_is_initialized);
  ASSERT (t != NULL);
  //printf ("   CLEANE VM VON %8p.\n", t);
    
  lock_acquire (&vm_lock);
  enum intr_level old_level = intr_disable ();
  
  hash_destroy (&t->vm_pages, &vm_clean_sub);
  
  lock_release (&vm_lock);
  intr_set_level (old_level);
}

bool
vm_alloc_zero (struct thread *t, void *addr, bool readonly)
{
  assert_t_addr (t, addr);
  
  bool result;
  
  bool outer_lock = lock_held_by_current_thread (&vm_lock);
  if (!outer_lock)
    lock_acquire (&vm_lock);
  enum intr_level old_level = intr_disable ();
  
  //printf ("   ALLOC ZERO: %8p\n", addr);
  
  struct vm_page *page = calloc (1, sizeof (*page));
  if (!page)
    {
      result = false;
      goto end;
    }
    
  page->type       = VMPPT_EMPTY;
  page->thread     = t;
  page->user_addr  = addr;
  page->vmlp_magic = VMLP_MAGIC;
  page->readonly   = !!readonly;
  
  hash_insert (&t->vm_pages, &page->thread_elem);
  
  result = true;
  
end:
  if (!outer_lock)
    lock_release (&vm_lock);
  intr_set_level (old_level);
  return result;
}

static struct vm_page *
vm_get_logical_page (struct thread *t, void *user_addr)
{
  assert_t_addr (t, user_addr);
  ASSERT (lock_held_by_current_thread (&vm_lock));
  
  struct vm_page key;
  key.user_addr = user_addr;
  key.thread = t;
  key.vmlp_magic = VMLP_MAGIC;
  
  struct hash_elem *e = hash_find (&t->vm_pages, &key.thread_elem);
  return vmlp_entry (e,t);
}

/* Called when swap needed room and disposed an unchanged page.
 * Not called when disposal was initiated through swap_dispose|swap_clean.
 * Called once per disposed page.
 * Might be called with interrupts on!
 */
void
vm_swap_disposed (struct thread *t, void *user_addr)
{
  assert_t_addr (t, user_addr);
  lock_held_by_current_thread (&vm_lock);
  
  // With being inside swaps lock, page cannot have been free'd.
  // No need to disable interrupts.
  struct vm_page *ee = vm_get_logical_page (t, user_addr);
  ASSERT (ee != NULL);
  
  ASSERT (ee->type == VMPPT_SWAPPED);
  ee->type = VMPPT_USED;
}

void
vm_tick (struct thread *t)
{
  ASSERT (t != NULL);
  ASSERT (t->pagedir != NULL);
  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (intr_context ());
  
  if (!vm_is_initialized)
    return;
  
  // TODO: lru for accessed pages
  // TODO: swap disposing for dirty pages
}

static bool
swap_free_page (void)
{
  ASSERT (intr_get_level () == INTR_ON);
  ASSERT (lock_held_by_current_thread (&vm_lock));
  
  bool result = false;
  
  // No other swap operation, except swap_tick, can disturb this flow,
  // b/c of vm_lock.
  //
  // Schema:
  //
  // NO INTERRUPTS:
  //   1) Search least recently accessed user page A, with kernel page K.
  //   2) If A was swapped in
  //     2.1) IF NOT DIRTY A:
  //       2.1.1) Remove dispose A and free K.
  //       2.1.2) Remove A from LRU list.
  //       2.2.3) Tell swap A was swapped out and cannot be disposed.
  //     2.2) Otherwise:
  //       2.2.1) Mark A as not dirty, tell LRU A was used.
  //       2.2.2) Tell swap A is dirty.
  //       2.2.3) Go back to (1).
  //   3) Dispose A, remove A from LRU list, mark it as swapped out.
  //      (Dispose A here, so that another thread had an page fault,
  //      if referencing A.)
  // INTERRUPTABLE:
  //   4) Try allocate swap space and write K to it.
  // NO INTERRUPTS:
  //   IF (4) WORKED:
  //     5.1.1) Free K.
  //   OTHERWISE:
  //     5.2.1) Set A -> K in pagedir again.
  
  intr_disable ();
  
  struct vm_page *ee;
  void *kpage;
  for (;;)
    {
      // (1)
      struct lru_elem *e = lru_peek_least (&pages_lru);
      if (!e)
        goto end;
      ee = lru_entry (e, struct vm_page, lru_elem);
      kpage = pagedir_get_page (ee->thread->pagedir, ee->user_addr);
      ASSERT (kpage != NULL);
      
      // (2)
      if (ee->type == VMPPT_SWAPPED)
        {
          if(!pagedir_is_dirty (ee->thread->pagedir, ee->user_addr))
            {
              // (2.1.1)
              pagedir_clear_page (ee->thread->pagedir, ee->user_addr);
              palloc_free_page (kpage);
              // (2.1.2)
              lru_dispose (&pages_lru, &ee->lru_elem, false);
              // (2.1.3)
              swap_must_retain (ee->thread, ee->user_addr);
              result = true;
              goto end;
            }
          else
            {
              // Page is dirty. Try some other page.
              // (2.2.1)
              pagedir_set_dirty (ee->thread->pagedir, ee->user_addr, false);
              lru_use (&pages_lru, &ee->lru_elem);
              ee->type = VMPPT_USED;
              // (2.2.2)
              bool swap_result UNUSED;
              swap_result = swap_dispose (ee->thread, ee->user_addr);
              ASSERT (swap_result);
              // (2.2.3)
              continue;
            }
        }
      
      break;
    }
  
  // (3)
  pagedir_clear_page (ee->thread->pagedir, ee->user_addr);
  lru_dispose (&pages_lru, &ee->lru_elem, false);
  ee->type = VMPPT_SWAPPED;
  
  // (4)
  intr_enable ();
  result = swap_alloc_and_write (ee->thread, ee->user_addr, kpage);
  ASSERT (result);
  intr_disable ();
  
  if (result) // (5.1.1)
    {
      palloc_free_page (kpage);
      swap_must_retain (ee->thread, ee->user_addr);
    }
  else // (5.2.1)
    pagedir_set_page (ee->thread->pagedir, ee->user_addr, kpage, true);
  
end:
  intr_enable();
  return result;
}

static bool
swap_free_memory (void)
{
  ASSERT (intr_get_level () == INTR_ON);
  ASSERT (lock_held_by_current_thread (&vm_lock));
  
  size_t freed = 0;
  while (freed < swap_at_once && !lru_is_empty (&pages_lru))
    {
      if (swap_free_page ())
        ++freed;
      else
        break;
    }
  return freed > 0;
}

static void *
vm_alloc_kpage (struct vm_page *ee)
{
  ASSERT (ee != NULL);
  ASSERT (ee->user_addr != NULL);
  ASSERT (ee->thread != NULL);
  ASSERT (ee->vmlp_magic == VMLP_MAGIC);
  ASSERT (lock_held_by_current_thread (&vm_lock));
  ASSERT (pagedir_get_page (ee->thread->pagedir, ee->user_addr) == NULL);
  ASSERT (intr_get_level () == INTR_ON);
  
  void *kpage = vm_palloc ();
  if (kpage == NULL)
    return NULL;
  
  int retry;
  for (retry = MAX_ALLOC_RETRIES; retry > 0; --retry)
    {
      if (pagedir_set_page (ee->thread->pagedir, ee->user_addr, kpage,
                            !ee->readonly))
        return kpage;
      if (!swap_free_memory ())
        break;
    }
  palloc_free_page (kpage);
  return NULL;
}

void *
vm_palloc (void)
{
  ASSERT (intr_get_level () == INTR_ON);
  
  bool outer_lock = lock_held_by_current_thread (&vm_lock);
  if (!outer_lock)
    lock_acquire (&vm_lock);
  
  void *kpage = NULL;
  
  signed retry;
  for (retry = MAX_ALLOC_RETRIES; retry > 0; --retry)
    {
      kpage = palloc_get_page (PAL_USER);
      if (kpage != NULL)
        break;
      if (!swap_free_memory ())
        break;
    }
    
  if (!outer_lock)
    lock_release (&vm_lock);
  return kpage;
}

static bool
vm_real_alloc (struct vm_page *ee UNUSED, void **kpage_)
{
  ASSERT (kpage_ != NULL);
  ASSERT (*kpage_ != NULL);
  
  memset (*kpage_, 0, PGSIZE);
  return true;
}

static bool
vm_swap_in (struct vm_page *ee, void **kpage_)
{
  ASSERT (kpage_ != NULL);
  ASSERT (*kpage_ != NULL);
    
  bool result = swap_read_and_retain (ee->thread, ee->user_addr, *kpage_);
  ASSERT (result == true);
  return result;
}

enum vm_ensure_result
vm_ensure (struct thread *t, void *user_addr, void **kpage_)
{
  if (user_addr < MIN_ALLOC_ADDR || !is_user_vaddr (user_addr))
    return VMER_SEGV;
    
  assert_t_addr (t, user_addr);
  ASSERT (kpage_ != NULL);
  ASSERT (intr_get_level () == INTR_ON);
  
  bool outer_lock = lock_held_by_current_thread (&vm_lock);
  if (!outer_lock)
    lock_acquire (&vm_lock);
  
  enum vm_ensure_result result;
    
  struct vm_page *ee = vm_get_logical_page (t, user_addr);
  if (ee == NULL)
    {
      result = VMER_SEGV;
      goto end;
    }
  
  *kpage_ = pagedir_get_page (t->pagedir, user_addr);
  if (*kpage_ != NULL)
    {
      if (lru_is_interior (&ee->lru_elem))
        lru_use (&pages_lru, &ee->lru_elem);
      result = VMER_OK;
      goto end;
    }
  
  *kpage_ = vm_alloc_kpage (ee);
  if (!*kpage_)
    {
      result = VMER_OOM;
      goto end;
    }
  
  switch (ee->type)
    {
      case VMPPT_UNUSED:
        // VMPPT_UNUSED is a transient state and cannot occur
        PANIC ("ee->type == VMPPT_UNUSED");
        
      case VMPPT_EMPTY:
        result = vm_real_alloc (ee, kpage_) ? VMER_OK : VMER_OOM;
        break;
        
      case VMPPT_SWAPPED:
        result = vm_swap_in (ee, kpage_) ? VMER_OK : VMER_OOM;
        break;
      
      case VMPPT_USED:
      default:
        // VMPPT_USED implies pagedir_get_page != NULL
        PANIC ("ee->type == VMPPT_USED, but pagedir_get_page (...) == NULL");
    }
    
  if (result == VMER_OK)
    lru_use (&pages_lru, &ee->lru_elem);
  else
    {
      palloc_free_page (*kpage_);
      *kpage_ = NULL;
    }
    
end:
  if (!outer_lock)
    lock_release (&vm_lock);
  return result;
}

void
vm_dispose (struct thread *t, void *addr)
{
  assert_t_addr (t, addr);
  
  bool outer_lock = lock_held_by_current_thread (&vm_lock);
  if (!outer_lock)
    lock_acquire (&vm_lock);
    
  enum intr_level old_level = intr_disable ();
  
  struct vm_page *ee = vm_get_logical_page (t, addr);
  vm_dispose_real (ee);
  
  if (!outer_lock)
    lock_release (&vm_lock);
  
  intr_set_level (old_level);
}

void *
vm_alloc_and_ensure (struct thread *t, void *addr, bool readonly)
{
  assert_t_addr (t, addr);
  ASSERT (intr_get_level () == INTR_ON);
  
  lock_acquire (&vm_lock);
  
  if (!vm_alloc_zero (t, addr, readonly))
    {
      lock_release (&vm_lock);
      return NULL;
    }
  
  void *kpage;
  enum vm_ensure_result result = vm_ensure (t, addr, &kpage);
  if (result == VMER_OK)
    {
      lock_release (&vm_lock);
      return kpage;
    }
    
  ASSERT (result == VMER_OOM);
  vm_dispose (t, addr);
  lock_release (&vm_lock);
  
  return NULL;
}

enum vm_is_readonly_result
vm_is_readonly (struct thread *t, void *user_addr)
{
  assert_t_addr (t, user_addr);
  lock_acquire (&vm_lock);
  
  struct vm_page *ee = vm_get_logical_page (t, user_addr);
    
  lock_release (&vm_lock);
  return ee ? ee->readonly ? VMIR_READONLY : VMIR_READWRITE : VMIR_INVALID;
}

struct vm_ensure_group_entry
{
  struct hash_elem        elem;
  struct vm_page *page;
};

static struct vm_ensure_group_entry *
vm_ensure_group_get (struct vm_ensure_group  *g,
                     void                    *user_addr,
                     struct vm_page         **page_)
{
  ASSERT (lock_held_by_current_thread (&vm_lock));
  ASSERT (g != NULL);
  ASSERT (user_addr != NULL);
  ASSERT (page_ != NULL);
  ASSERT (pg_ofs (user_addr) == 0);
  
  *page_ = vm_get_logical_page (g->thread, user_addr);
  if (!*page_)
    return NULL;
  
  struct vm_ensure_group_entry key;
  memset (&key, 0, sizeof (key));
  key.page = *page_;
  
  struct hash_elem *e = hash_find (&g->entries, &key.elem);
  return hash_entry (e, struct vm_ensure_group_entry, elem);
}

static unsigned
vm_ensure_group_hash (const struct hash_elem *e, void *t)
{
  typedef char _CASSERT[0 - !(sizeof (unsigned) == sizeof (void *))];
  
  ASSERT (e != NULL);
  struct vm_ensure_group_entry *ee;
  ee = hash_entry (e, struct vm_ensure_group_entry, elem);
  ASSERT (ee->page != NULL);
  ASSERT (ee->page->thread == t);
  
  return (unsigned) ee->page;
}

static bool
vm_ensure_group_less (const struct hash_elem *a,
                      const struct hash_elem *b,
                      void *t)
{
  return vm_ensure_group_hash (a,t) < vm_ensure_group_hash (b,t);
}

void
vm_ensure_group_init (struct vm_ensure_group *g, struct thread *t)
{
  ASSERT (g != NULL);
  ASSERT (t != NULL);
  
  memset (g, 0, sizeof (*g));
  g->thread = t;
  hash_init (&g->entries, &vm_ensure_group_hash, &vm_ensure_group_less, t);
}

static void
vm_ensure_group_dispose_real (struct hash_elem *e, void *t)
{
  ASSERT (lock_held_by_current_thread (&vm_lock));
  ASSERT (intr_get_level () == INTR_OFF);
  
  ASSERT (e != NULL);
  struct vm_ensure_group_entry *ee;
  ee = hash_entry (e, struct vm_ensure_group_entry, elem);
  ASSERT (ee->page != NULL);
  ASSERT (ee->page->thread == t);
  ASSERT (ee->page->vmlp_magic == VMLP_MAGIC);
  
  lru_use (&pages_lru, &ee->page->lru_elem);
    
  free (ee);
}

void
vm_ensure_group_destroy (struct vm_ensure_group *g)
{
  ASSERT (g != NULL);
  
  lock_acquire (&vm_lock);
  enum intr_level old_level = intr_disable ();
  
  hash_destroy (&g->entries, &vm_ensure_group_dispose_real);
  
  lock_release (&vm_lock);
  intr_set_level (old_level);
}

enum vm_ensure_result
vm_ensure_group_add (struct vm_ensure_group *g, void *user_addr, void **kpage_)
{
  ASSERT (g != NULL);
  ASSERT (user_addr != NULL);
  
  lock_acquire (&vm_lock);
    
  enum vm_ensure_result result = vm_ensure (g->thread, user_addr, kpage_);
  if (result != VMER_OK)
    {
      PANIC("FAIL FOR %8p", user_addr); // TODO: remove
      lock_release (&vm_lock);
      return result;
    }
  
  enum intr_level old_level = intr_disable ();
  
  struct vm_page *page;
  struct vm_ensure_group_entry *entry;
  entry = vm_ensure_group_get (g, user_addr, &page);
  if (entry != NULL)
    goto end;
  if (page == NULL)
    {
      ASSERT(0); // TODO: remove
      result = VMER_SEGV;
      goto end;
    }
  ASSERT (lru_is_interior (&page->lru_elem));
  entry = calloc (1, sizeof (*entry));
  entry->page = page;
  hash_insert (&g->entries, &entry->elem);
  lru_dispose (&pages_lru, &page->lru_elem, false);
  
end:
  lock_release (&vm_lock);
  intr_set_level (old_level);
  return VMER_OK;
}

bool
vm_ensure_group_remove (struct vm_ensure_group *g, void *user_addr)
{
  ASSERT (g != NULL);
  ASSERT (user_addr != NULL);
  
  lock_acquire (&vm_lock);
  enum intr_level old_level = intr_disable ();
  
  struct vm_page *page;
  struct vm_ensure_group_entry *entry;
  entry = vm_ensure_group_get (g, user_addr, &page);
  if (entry != NULL)
    {
      hash_delete_found (&g->entries, &entry->elem);
      vm_ensure_group_dispose_real (&entry->elem, g->thread);
      lock_release (&vm_lock);
      intr_set_level (old_level);
      return true;
    }
    
  lock_release (&vm_lock);
  intr_set_level (old_level);
  return page != NULL;
}
