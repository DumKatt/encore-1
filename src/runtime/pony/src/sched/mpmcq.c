#include "mpmcq.h"
#include "../mem/pool.h"

typedef struct mpmcq_node_t mpmcq_node_t;

struct mpmcq_node_t
{
  mpmcq_node_t* next;
  void* data;
};

void mpmcq_init( mpmcq_t* q )
{
  mpmcq_node_t* node = POOL_ALLOC(mpmcq_node_t);
  node->data = NULL;
  node->next = NULL;

  q->head = node;
  q->tail.node = node;
}

void mpmcq_destroy(mpmcq_t* q)
{
  POOL_FREE(mpmcq_node_t, q->tail.node);
  q->head = NULL;
  q->tail.node = NULL;
}

void mpmcq_push(mpmcq_t* q, void* data)
{
  mpmcq_node_t* node = POOL_ALLOC(mpmcq_node_t);
  node->data = data;
  node->next = NULL;

  mpmcq_node_t* prev = (mpmcq_node_t*) __pony_atomic_exchange_n(
    &q->head, node, PONY_ATOMIC_RELAXED, intptr_t);

  __pony_atomic_store_n(&prev->next, node, PONY_ATOMIC_RELEASE,
    PONY_ATOMIC_NO_TYPE);
}

void* mpmcq_pop(mpmcq_t* q)
{
  mpmcq_dwcas_t cmp, xchg;
  mpmcq_node_t* next;
  void* data;

  cmp.dw = q->tail.dw;

  do
  {
    next = (mpmcq_node_t*)__pony_atomic_load_n(&cmp.node->next,
      PONY_ATOMIC_ACQUIRE, PONY_ATOMIC_NO_TYPE);

    if(next == NULL)
      return NULL;

    data = next->data;
    xchg.node = next;
    xchg.aba = cmp.aba + 1;
  } while(!__pony_atomic_compare_exchange_n(&q->tail.dw, &cmp.dw, xchg.dw,
    false, PONY_ATOMIC_RELAXED, PONY_ATOMIC_RELAXED, __int128_t));

  POOL_FREE(mpmcq_node_t, cmp.node);
  return data;
}
