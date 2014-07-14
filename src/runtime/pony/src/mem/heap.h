#ifndef mem_heap_h
#define mem_heap_h

#include <pony/pony.h>
#include <stdlib.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HEAP_MINBITS 6
#define HEAP_MAXBITS 11
#define HEAP_SIZECLASSES (HEAP_MAXBITS - HEAP_MINBITS + 1)

typedef struct chunk_t chunk_t;

typedef struct heap_t
{
  chunk_t* small[HEAP_SIZECLASSES];
  chunk_t* small_full[HEAP_SIZECLASSES];
  chunk_t* large;

  size_t used;
  size_t next_gc;
} heap_t;

void heap_init(heap_t* heap);

void heap_destroy(heap_t* heap);

void* heap_alloc(pony_actor_t* actor, heap_t* heap, size_t size)
  __attribute__((malloc,alloc_size(3)));

/**
 * Adds to the used memory figure kept by the heap. This allows objects received
 * in messages to count towards the GC heuristic.
 */
void heap_used(heap_t* heap, size_t size);

bool heap_startgc(heap_t* heap);

/**
 * Mark an address in a chunk. Returns true if it was already marked, or false
 * if you have just marked it.
 */
bool heap_mark(chunk_t* chunk, void* p);

/**
 * Marks an address, but does not affect the return value of heap_mark() for the
 * same address, nor does it indicate previous mark status.
 */
void heap_mark_shallow(chunk_t* chunk, void* p);

void heap_endgc(heap_t* heap);

pony_actor_t* heap_owner(chunk_t* chunk);

/**
 * Given a pointer to somewhere internal to a block of allocated memory, this
 * adjusts it to be the base pointer to the block and returns the size of the
 * block.
 */
size_t heap_base(chunk_t* chunk, void** p);

#ifdef __cplusplus
}
#endif

#endif
