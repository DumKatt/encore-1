#define _XOPEN_SOURCE 800
#include <ucontext.h>

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

#include <assert.h>
#include <pthread.h>
#include <pony/pony.h>

#include "encore.h"
#include "future.h"
#include "../src/actor/messageq.h"

#define BLOCK    pthread_mutex_lock(&fut->lock);
#define UNBLOCK  pthread_mutex_unlock(&fut->lock);
#define perr(m)  // fprintf(stderr, "%s\n", m);

extern pony_actor_t *actor_current();

typedef struct chain_entry chain_entry_t;
typedef struct actor_entry actor_entry_t;
typedef struct closure_entry closure_entry_t;
typedef struct message_entry message_entry_t;

// Terminology:
// Producer -- the actor responsible for fulfilling a future
// Consumer -- an non-producer actor using a future

typedef enum
{
  // A closure that should be run by the producer
  DETACHED_CLOSURE,
  // A closure that should be run ty the consumer
  ATTACHED_CLOSURE,
  // A message blocked on this future
  BLOCKED_MESSAGE
} responsibility_t;

struct closure_entry
{
  // The consumer that created closure
  pony_actor_t *actor;
  // The future where the result of the closure should be stored
  future_t     *future;
  // The closure to be run on fulfilment of the future
  closure_t    *closure;

  closure_entry_t *next;
};

struct message_entry
{
  // The consumer that created closure
  pony_actor_t *actor;
  // FIXME: add context
};

struct actor_entry
{
  responsibility_t type;
  union
  {
    closure_entry_t closure;
    message_entry_t message;
  };
};

struct future
{
  encore_arg_t      value;
  encore_type_t    *type;
  bool            fulfilled;
  // Stupid limitation for now
  actor_entry_t   responsibilities[16];
  int             no_responsibilities;
  // Lock-based for now
  pthread_mutex_t lock;
  future_t *parent;
  closure_t *closure;
  closure_entry_t *children;
};

static inline void future_gc_send(future_t *fut);
static inline void future_gc_recv(future_t *fut);

encore_type_t future_type = {
  {
    ID_FUTURE,
    sizeof(struct future),
    future_trace,
    NULL,
    NULL,
    NULL,
    NULL
  },
  "Future"
};

encore_type_t *future_get_type(future_t *fut){
  return fut->type;
}

void future_trace(void* p)
{
  /// TODO: find out what should actually be traced in a future.
    return;

  /// Currently this does not block -- as futures grow monotonically,
  /// concurrent access should not be a problem.
  perr("future_trace");

  future_t* fut = p;

  if (future_fulfilled(fut))
    {
      if (fut->type == ENCORE_ACTIVE)
        {
          // Fut ActiveObj
          pony_traceactor(fut->value.p);
        }
      else if (fut->type != ENCORE_PRIMITIVE)
        {
          // Fut PassiveObj
          pony_traceobject(fut->value.p, fut->type->trace);
        }
    }

  for (int i = 0; i < fut->no_responsibilities; ++i) {
    pony_traceactor(fut->responsibilities[i].message.actor);
  }

  for (closure_entry_t *c = fut->children; c; c = c->next) {
    pony_traceactor(c->actor);
    future_trace(c->future);
    closure_trace(c->closure);
  }

  if (fut->closure) closure_trace(fut->closure);

  if (fut->parent) future_trace(fut->parent);
}

// ===============================================================
// Create, inspect and fulfil
// ===============================================================
future_t *future_mk(encore_type_t *type)
{
  perr("future_mk");

  future_t *fut = encore_alloc(sizeof(future_t));
  *fut = (future_t) { .type = type };

  pthread_mutex_init(&fut->lock, NULL);
  return fut;
}

encore_arg_t run_closure(closure_t *c, encore_arg_t value, future_t *fut)
{
  value_t result = closure_call(c, (value_t[1]) { value });
  future_fulfil(fut, result);
  return result;
}

bool future_fulfilled(future_t *fut)
{
  perr("future_fulfilled");
  bool r;
  BLOCK;
  r = fut->fulfilled;
  UNBLOCK;
  return r;
}

encore_arg_t future_read_value(future_t *fut)
{
  perr("future_read_value");
  BLOCK;
  encore_arg_t v = fut->value;
  UNBLOCK;
  return v;
}

void future_fulfil(future_t *fut, encore_arg_t value)
{
  perr("future_fulfil");

  BLOCK;
  fut->value = value;
  fut->fulfilled = true;

  // future_gc_send(fut);

  // Unblock on blocked actors
  // TODO chained futures not handled yet
  closure_entry_t *current = fut->children;
  bool blocked;
  while(current) {
    blocked = false;
    // TODO use one set for blocked actors
    for (int i = 0; i < fut->no_responsibilities; ++i) {
      if(fut->responsibilities[i].message.actor == current->actor) {
        blocked = true;
        break;
      }
    }
    if (!blocked) {
      encore_arg_t argv[3] = { { .p = current->closure }, value, { .p = current->future } };
      // TODO how to send three args
      // pony_sendv(current->actor, FUT_MSG_RUN_CLOSURE, 3, argv);
    }
    current = current->next;
  }

  for (int i = 0; i < fut->no_responsibilities; ++i) {
    actor_entry_t e = fut->responsibilities[i];
    switch (e.type)
    {
      case BLOCKED_MESSAGE:
        {
          perr("Unblocking");
          actor_set_resume((encore_actor_t*)e.message.actor);
          pony_schedule(e.message.actor);
          break;
        }
        // Current design: send closure back to origin to execute (deadlock-prone)
        // Intended design: see https://github.com/parapluu/mylittlepony/wiki/Futures
      case ATTACHED_CLOSURE:
        {
          encore_arg_t argv[3] = { { .p = e.closure.closure }, value, { .p = e.closure.future } };
          // pony_sendv(e.closure.actor, FUT_MSG_RUN_CLOSURE, 3, argv);
          break;
        }
        // Design 1: current thread executes closures (racy)
      case DETACHED_CLOSURE:
        {
          value_t result = closure_call(e.closure.closure, (value_t[1]) { value });
          future_fulfil(e.closure.future, result);
          break;
        }
    }
  }
  UNBLOCK;
}

// ===============================================================
// Means for actors to get, block and chain
// ===============================================================
encore_arg_t future_get_actor(future_t *fut)
{
  // early return for simple case
  if (fut->parent == NULL) {
    future_block_actor(fut);

    // future_gc_recv(fut);
    return fut->value;
  }

  if (!future_fulfilled(fut->parent)) {
    future_block_actor(fut->parent);
  }

  /// TODO: in this case, we need to run future_gc_recv() too!
  return run_closure(fut->closure, future_read_value(fut->parent), fut);
}

future_t  *future_chain_actor(future_t *fut, future_t* r, closure_t *c)
{
  perr("future_chain_actor");

  closure_entry_t *entry = encore_alloc(sizeof *entry);
  entry->actor = actor_current();
  entry->future = r;
  entry->closure = c;

  BLOCK;
  entry->next = fut->children;
  fut->children = entry;
  UNBLOCK;

  r->parent = fut;
  r->closure = c;

  return r;
}

void future_block_actor(future_t *fut)
{
  perr("future_block_actor");

  pony_actor_t *a = actor_current();
  BLOCK;

  if (fut->fulfilled) {
    UNBLOCK;
    return;
  }

  // the implementation of messageq_find is specific to this case.
  // therefore, it's not thread-safe
  bool block_myself = false; /// messageq_find(pony_get_messageq(), fut);
  if(block_myself){
    UNBLOCK;
    /// actor_suspend(a);
  }else{
    pony_unschedule(a);
    fut->responsibilities[fut->no_responsibilities++] = (actor_entry_t) { .type = BLOCKED_MESSAGE, .message = (message_entry_t) { .actor = a } };
    UNBLOCK;

    actor_block((encore_actor_t*)a);
  }

}

void future_unblock_actor(future_t *r)
{
  perr("future_unblock_actor");

  // FIXME: move context into message
  /// actor_resume(actor_current());
}

// ===============================================================
// Possibly these functions do not belong in the future library
// ===============================================================
void future_suspend(void)
{
  // FIXME: move context into message
  // TODO: block GC'ing during suspend
  /// actor_suspend(actor_current());
}

// FIXME: better type for this
void future_suspend_resume(void *arg)
{
  ctx_wrapper *d = (ctx_wrapper *) arg;
  ucontext_t* ctx = d->ctx;
  ctx->uc_link = d->uc_link;

  pony_actor_t *actor = actor_current();
  /// actor_set_run_to_completion(actor);

  assert(swapcontext(ctx->uc_link, ctx) == 0);


  /// if (actor_run_to_completion(actor)) {
  ///     reclaim_page(actor);
  /// }
}

void future_await(future_t *fut)
{
  /// actor_await(actor_current(), fut);
}

// FIXME: better type for this
void future_await_resume(void *argv)
{
  ctx_wrapper *d = ((encore_arg_t *)argv)[0].p;
  ucontext_t* ctx = d->ctx;
  ctx->uc_link = d->uc_link;

  future_t *fut = ((encore_arg_t *)argv)[1].p;

  if (future_fulfilled(fut))
  {
    pony_actor_t *actor = actor_current();
    /// actor_set_run_to_completion(actor);

    assert(swapcontext(ctx->uc_link, ctx) == 0);

    /// if (actor_run_to_completion(actor)) {
    ///     reclaim_page(actor);
    /// }
  } else {
    // pony_sendv(actor_current(), FUT_MSG_AWAIT, 2, argv);
  }
}

static inline void future_gc_trace(future_t *fut)
{
  if (fut->type == ENCORE_ACTIVE)
    {
      pony_traceactor(fut->value.p);
    }
  else if (fut->type == ENCORE_PRIMITIVE)
    {
      // Tracing not needed for primitives
    }
  else
    {
      pony_traceobject(fut->value.p, fut->type->trace);
    }
}

static inline void future_gc_send(future_t *fut)
{
  /// These probably do not do the right thing
  pony_gc_send();
  future_gc_trace(fut);
  pony_send_done();
}

static inline void future_gc_recv(future_t *fut)
{
  /// These probably do not do the right thing
  pony_gc_recv();
  future_gc_trace(fut);
  pony_recv_done();
}
