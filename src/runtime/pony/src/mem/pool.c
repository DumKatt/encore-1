#include "pool.h"
#include "../ds/fun.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <assert.h>

#define POOL_MAX_BITS 21
#define POOL_MAX (1 << POOL_MAX_BITS)
#define POOL_MIN (1 << POOL_MIN_BITS)
#define POOL_COUNT (POOL_MAX_BITS - POOL_MIN_BITS)
#define POOL_THRESHOLD (POOL_MAX >> 1)
#define POOL_MMAP (POOL_MAX << 2)

typedef __int128_t pool_aba_t;

typedef struct pool_item_t
{
  struct pool_item_t* next;
} pool_item_t;

typedef struct pool_local_t
{
  pool_item_t* pool;
  size_t length;
  void* start;
  void* end;
} pool_local_t;

typedef struct pool_global_t
{
  size_t size;
  size_t count;
  pool_aba_t central;
} pool_global_t;

typedef struct pool_central_t
{
  pool_item_t* next;
  uintptr_t length;
  struct pool_central_t* central;
} pool_central_t;

typedef struct pool_cmp_t
{
  union
  {
    struct
    {
      uint64_t aba;
      pool_central_t* node;
    };

    pool_aba_t dw;
  };
} pool_cmp_t;

static pool_global_t pool_global[POOL_COUNT] =
{
  {POOL_MIN << 0, POOL_MAX / (POOL_MIN << 0), 0},
  {POOL_MIN << 1, POOL_MAX / (POOL_MIN << 1), 0},
  {POOL_MIN << 2, POOL_MAX / (POOL_MIN << 2), 0},
  {POOL_MIN << 3, POOL_MAX / (POOL_MIN << 3), 0},
  {POOL_MIN << 4, POOL_MAX / (POOL_MIN << 4), 0},
  {POOL_MIN << 5, POOL_MAX / (POOL_MIN << 5), 0},
  {POOL_MIN << 6, POOL_MAX / (POOL_MIN << 6), 0},
  {POOL_MIN << 7, POOL_MAX / (POOL_MIN << 7), 0},
  {POOL_MIN << 8, POOL_MAX / (POOL_MIN << 8), 0},
  {POOL_MIN << 9, POOL_MAX / (POOL_MIN << 9), 0},
  {POOL_MIN << 10, POOL_MAX / (POOL_MIN << 10), 0},
  {POOL_MIN << 11, POOL_MAX / (POOL_MIN << 11), 0},
  {POOL_MIN << 12, POOL_MAX / (POOL_MIN << 12), 0},
  {POOL_MIN << 13, POOL_MAX / (POOL_MIN << 13), 0},
  {POOL_MIN << 14, POOL_MAX / (POOL_MIN << 14), 0},
};

// TODO: free on thread exit
static __thread pool_local_t pool_local[POOL_COUNT];

static pool_item_t* pool_block(pool_local_t* thread, pool_global_t* global)
{
  if(thread->start < thread->end)
  {
    pool_item_t* p = thread->start;
    thread->start += global->size;
    return p;
  }

  return NULL;
}

static void pool_push(pool_local_t* thread, pool_global_t* global)
{
  pool_cmp_t cmp, xchg;
  pool_central_t* p = (pool_central_t*)thread->pool;
  p->length = thread->length;
  cmp.dw = global->central;

  do
  {
    p->central = cmp.node;
    xchg.node = p;
    xchg.aba = cmp.aba + 1;
  } while(!__atomic_compare_exchange_n(&global->central, &cmp.dw, xchg.dw,
    false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));

  thread->pool = NULL;
  thread->length = 0;
}

static pool_item_t* pool_pull(pool_local_t* thread, pool_global_t* global)
{
  pool_cmp_t cmp, xchg;
  pool_central_t* next;
  cmp.dw = global->central;

  do
  {
    next = cmp.node;

    if(next == NULL)
      return NULL;

    xchg.node = next->central;
    xchg.aba = cmp.aba + 1;
  } while(!__atomic_compare_exchange_n(&global->central, &cmp.dw, xchg.dw,
    false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));

  pool_item_t* p = (pool_item_t*)next;
  thread->pool = p->next;
  thread->length = next->length - 1;

  return p;
}

static void* pool_mmap(size_t size)
{
  void* p = mmap(
    0,
    size,
    PROT_READ | PROT_WRITE,
    MAP_PRIVATE | MAP_ANON,
    -1,
    0
    );

  if(p == MAP_FAILED)
    abort();

  return p;
}

static void pool_munmap(size_t size, void* p)
{
  if(munmap(p, size) != 0)
    abort();
}

static pool_item_t* pool_pages(pool_local_t* thread, pool_global_t* global)
{
  void* p = pool_mmap(POOL_MMAP);
  thread->start = p + global->size;
  thread->end = p + POOL_MMAP;
  return p;
}

void* pool_alloc(int index)
{
  pool_local_t* thread = &pool_local[index];
  pool_global_t* global = &pool_global[index];
  pool_item_t* p = thread->pool;

  if(p != NULL)
  {
    thread->pool = p->next;
    thread->length--;
    return p;
  }

  if((p = pool_block(thread, global)) != NULL)
    return p;

  if((p = pool_pull(thread, global)) != NULL)
    return p;

  return pool_pages(thread, global);
}

void pool_free(int index, void* p)
{
  pool_local_t* thread = &pool_local[index];
  pool_global_t* global = &pool_global[index];

  if(thread->length >= global->count)
    pool_push(thread, global);

  pool_item_t* lp = p;
  lp->next = thread->pool;
  thread->pool = p;
  thread->length++;
}

void* pool_alloc_size(size_t size)
{
  if(size <= POOL_MIN)
    return pool_alloc(0);

  if(size <= POOL_THRESHOLD)
  {
    size = next_pow2(size);
    return pool_alloc(__builtin_ffsl(size) - (POOL_MIN_BITS + 1));
  }

  return pool_mmap(size);
}

void pool_free_size(size_t size, void* p)
{
  if(size <= POOL_MIN)
    return pool_free(0, p);

  if(size <= POOL_THRESHOLD)
  {
    size = next_pow2(size);
    pool_free(__builtin_ffsl(size) - (POOL_MIN_BITS + 1), p);
    return;
  }

  pool_munmap(size, p);
}

bool pool_debug_appears_freed()
{
  for(int i = 0; i < POOL_COUNT; i++)
  {
    pool_local_t* thread = &pool_local[i];
    pool_global_t* global = &pool_global[i];

    size_t avail = (thread->length * global->size) +
      (thread->end - thread->start);

    if((avail != 0) && (avail != POOL_MAX))
      return false;

    pool_cmp_t cmp;
    cmp.dw = global->central;
    pool_central_t* next = cmp.node;

    while(next != NULL)
    {
      if(next->length != global->count)
        return false;

      next = next->central;
    }
  }

  return true;
}
