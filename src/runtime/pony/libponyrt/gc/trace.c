#include "trace.h"
#include "gc.h"
#include "../actor/actor.h"

typedef void (*trace_object_fn)(pony_actor_t* current, heap_t* heap, gc_t* gc,
  void* p, pony_trace_fn f);

typedef void (*trace_actor_fn)(pony_actor_t* current, heap_t* heap, gc_t* gc,
  pony_actor_t* actor);

static __pony_thread_local trace_object_fn trace_object;
static __pony_thread_local trace_actor_fn trace_actor;

void pony_gc_send()
{
  trace_object = gc_sendobject;
  trace_actor = gc_sendactor;
}

void pony_gc_recv()
{
  trace_object = gc_recvobject;
  trace_actor = gc_recvactor;
}

void pony_gc_mark()
{
  trace_object = gc_markobject;
  trace_actor = gc_markactor;
}

void pony_gc_acquire()
{
  trace_object = gc_acquireobject;
  trace_actor = gc_acquireactor;
}

void pony_acquire_done()
{
  pony_actor_t* actor = actor_current();
  gc_handlestack();
  gc_sendacquire();
  gc_done(actor_gc(actor));
}

void pony_send_done()
{
  pony_actor_t* actor = actor_current();
  gc_handlestack();
  gc_sendacquire();
  gc_done(actor_gc(actor));
}

void pony_recv_done()
{
  pony_actor_t* actor = actor_current();
  gc_handlestack();
  gc_done(actor_gc(actor));
}

void pony_trace(void* p)
{
  pony_actor_t* actor = actor_current();
  trace_object(actor, actor_heap(actor), actor_gc(actor), p, NULL);
}

void pony_traceactor(pony_actor_t* p)
{
  pony_actor_t* actor = actor_current();
  trace_actor(actor, actor_heap(actor), actor_gc(actor), p);
}

void pony_traceobject(void* p, pony_trace_fn f)
{
  pony_actor_t* actor = actor_current();
  trace_object(actor, actor_heap(actor), actor_gc(actor), p, f);
}

void pony_traceunknown(void* p)
{
  pony_actor_t* actor = actor_current();
  pony_type_t* type = *(pony_type_t**)p;

  if(type->dispatch != NULL)
  {
    trace_actor(actor, actor_heap(actor), actor_gc(actor), (pony_actor_t*)p);
  } else {
    trace_object(actor, actor_heap(actor), actor_gc(actor), p, type->trace);
  }
}

void pony_trace_tag_or_actor(void* p)
{
  pony_actor_t* actor = actor_current();
  pony_type_t* type = *(pony_type_t**)p;

  if(type->dispatch != NULL)
    trace_actor(actor, actor_heap(actor), actor_gc(actor), (pony_actor_t*)p);
  else
    trace_object(actor, actor_heap(actor), actor_gc(actor), p, NULL);
}
