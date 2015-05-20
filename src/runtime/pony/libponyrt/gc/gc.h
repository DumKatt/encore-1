#ifndef gc_gc_h
#define gc_gc_h

#include "objectmap.h"
#include "actormap.h"
#include "delta.h"
#include "../mem/heap.h"
#include <pony.h>
#include <platform.h>

#define GC_INC_MORE 256

PONY_EXTERN_C_BEGIN

typedef struct gc_t
{
  uint32_t mark;
  uint32_t rc_mark;
  size_t rc;
  size_t finalisers;
  objectmap_t local;
  actormap_t foreign;
  deltamap_t* delta;
} gc_t;

void gc_sendobject(pony_actor_t* actor, heap_t* heap, gc_t* gc,
  void* p, pony_trace_fn f);

void gc_recvobject(pony_actor_t* current, heap_t* heap, gc_t* gc,
  void* p, pony_trace_fn f);

void gc_markobject(pony_actor_t* current, heap_t* heap, gc_t* gc,
  void* p, pony_trace_fn f);

void gc_acquireobject(pony_actor_t* current, heap_t* heap, gc_t* gc,
  void* p, pony_trace_fn f);

void gc_acquireactor(pony_actor_t *current, heap_t* heap, gc_t *gc, pony_actor_t *actor);

void gc_sendactor(pony_actor_t* current, heap_t* heap, gc_t* gc,
  pony_actor_t* actor);

void gc_recvactor(pony_actor_t* current, heap_t* heap, gc_t* gc,
  pony_actor_t* actor);

void gc_markactor(pony_actor_t* current, heap_t* heap, gc_t* gc,
  pony_actor_t* actor);

void gc_createactor(heap_t* heap, gc_t* gc, pony_actor_t* actor);

void gc_handlestack();

void gc_sweep(gc_t* gc);

void gc_sendacquire();

void gc_sendrelease(gc_t* gc);

bool gc_acquire(gc_t* gc, actorref_t* aref);

bool gc_release(gc_t* gc, actorref_t* aref);

size_t gc_rc(gc_t* gc);

deltamap_t* gc_delta(gc_t* gc);

void gc_register_final(gc_t* gc, void* p, pony_final_fn final);

void gc_final(gc_t* gc);

void gc_done(gc_t* gc);

void gc_destroy(gc_t* gc);

PONY_EXTERN_C_END

#endif
