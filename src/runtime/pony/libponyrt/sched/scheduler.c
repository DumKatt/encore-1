#define _XOPEN_SOURCE 800
#include <ucontext.h>
#include "scheduler.h"
#include "cpu.h"
#include "mpmcq.h"
#include "../actor/actor.h"
#include "../gc/cycle.h"
#include "../asio/asio.h"
#include "../mem/pool.h"
#include "encore.h"
#include "task.h"
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <assert.h>

static DECLARE_THREAD_FN(run_thread);

extern uint32_t remaining_tasks;
extern void unset_unscheduled(pony_actor_t* a);
extern bool is_unscheduled(pony_actor_t*);
__thread encore_actor_t* this_encore_task;

extern bool pony_reschedule();

typedef enum
{
  SCHED_BLOCK,
  SCHED_UNBLOCK,
  SCHED_CNF,
  SCHED_ACK,
  SCHED_TERMINATE
} sched_msg_t;

// Scheduler global data.
static uint32_t scheduler_count;
static uint32_t context_waiting = 0;
static uint32_t thread_exit = 0;
static scheduler_t* scheduler;
static bool volatile detect_quiescence;
static bool use_mpmcq;
static bool use_yield;
static mpmcq_t inject;
static __pony_thread_local scheduler_t* this_scheduler;

// Forward declaration.
static void push(scheduler_t* sched, pony_actor_t* actor);

/**
 * Gets the next actor from the scheduler queue.
 */
static pony_actor_t* pop(scheduler_t* sched)
{
  if(use_mpmcq)
    return (pony_actor_t*)mpmcq_pop(&sched->q);

  pony_actor_t* actor = sched->tail;

  if(actor != NULL)
  {
    if(actor != sched->head)
    {
      sched->tail = actor_next(actor);
    } else {
      sched->head = NULL;
      sched->tail = NULL;
    }

    actor_setnext(actor, NULL);
  }

  return actor;
}

/**
 * Puts an actor on the scheduler queue.
 */
static void push(scheduler_t* sched, pony_actor_t* actor)
{
  if(use_mpmcq)
  {
    mpmcq_push_single(&sched->q, actor);
  } else {
    pony_actor_t* head = sched->head;

    if(head != NULL)
    {
      actor_setnext(head, actor);
      sched->head = actor;
    } else {
      sched->head = actor;
      sched->tail = actor;
    }
  }
}

/**
 * Puts an actor on the scheduler queue in the beginning.
 */
static void push_first(scheduler_t* sched, pony_actor_t* actor)
{
  if(use_mpmcq) {
    puts("push_first not support used with mpmcq");
    assert(0);
    // mpmcq_push_single(&sched->q, actor);
  } else {
    pony_actor_t* tail = sched->tail;

    if(tail != NULL)
    {
      actor_setnext(actor, tail);
      sched->tail = actor;
    } else {
      sched->head = actor;
      sched->tail = actor;
    }
  }
}

/**
 * Handles the global queue and then pops from the local queue
 */
static pony_actor_t* pop_global(scheduler_t* sched)
{
  pony_actor_t* actor = (pony_actor_t*)mpmcq_pop(&inject);

  if(actor != NULL)
    return actor;

  return pop(sched);
}

/**
 * Sends a message to a thread.
 */
static void send_msg(uint32_t to, sched_msg_t msg, uint64_t arg)
{
  pony_msgi_t* m = (pony_msgi_t*)pony_alloc_msg(
    POOL_INDEX(sizeof(pony_msgi_t)), msg);

  m->i = arg;
  messageq_push(&scheduler[to].mq, &m->msg);
}

static void read_msg(scheduler_t* sched)
{
  pony_msgi_t* m;

  while((m = (pony_msgi_t*)messageq_pop(&sched->mq)) != NULL)
  {
    switch(m->msg.id)
    {
      case SCHED_BLOCK:
      {
        sched->block_count++;

        if(detect_quiescence && (sched->block_count == scheduler_count))
        {
          // If we think all threads are blocked, send CNF(token) to everyone.
          for(uint32_t i = 0; i < scheduler_count; i++)
            send_msg(i, SCHED_CNF, sched->ack_token);
        }
        break;
      }

      case SCHED_UNBLOCK:
      {
        // Cancel all acks and increment the ack token, so that any pending
        // acks in the queue will be dropped when they are received.
        sched->block_count--;
        sched->ack_token++;
        sched->ack_count = 0;
        break;
      }

      case SCHED_CNF:
      {
        // Echo the token back as ACK(token).
        send_msg(0, SCHED_ACK, m->i);
        break;
      }

      case SCHED_ACK:
      {
        // If it's the current token, increment the ack count.
        if(m->i == sched->ack_token)
          sched->ack_count++;
        break;
      }

      case SCHED_TERMINATE:
      {
        sched->terminate = true;
        break;
      }

      default: {}
    }
  }
}

/**
 * If we can terminate, return true. If all schedulers are waiting, one of
 * them will stop the ASIO back end and tell the cycle detector to try to
 * terminate.
 */
static bool quiescent(scheduler_t* sched, uint64_t tsc)
{
  read_msg(sched);

  if(sched->terminate)
    return true;

  if(sched->ack_count == scheduler_count)
  {
    if(sched->asio_stopped)
    {
      // ASIO has already been stopped.
      if(!use_mpmcq)
      {
        // It's safe to manipulate our victim, since we know it's paused.
        if(sched->victim != NULL)
          _atomic_store(&sched->victim->thief, NULL);

        _atomic_store(&sched->waiting, 0);
      }

      // Reset the ACK token in case we are rescheduling ourself.
      cycle_terminate(sched->forcecd);
      sched->forcecd = false;
      sched->ack_token++;
      sched->ack_count = 0;
    } else if(asio_stop()) {
      sched->asio_stopped = true;
      sched->ack_token++;
      sched->ack_count = 0;

      // Run another CNF/ACK cycle.
      for(uint32_t i = 0; i < scheduler_count; i++)
        send_msg(i, SCHED_CNF, sched->ack_token);
    }
  }

  cpu_core_pause(tsc, use_yield);
  return false;
}

static scheduler_t* choose_victim(scheduler_t* sched)
{
  scheduler_t* victim = sched->last_victim;

  while(true)
  {
    // Back up one.
    victim--;

    // Wrap around to the end.
    if(victim < scheduler)
      victim = &scheduler[scheduler_count - 1];

    if(victim == sched->last_victim)
    {
      // If we have tried all possible victims, return no victim. Set our last
      // victim to ourself to indicate we've started over.
      sched->last_victim = sched;
      break;
    }

    // Don't try to steal from ourself.
    if(victim == sched)
      continue;

    if(!use_mpmcq)
    {
      scheduler_t* thief = NULL;

      // Mark that we are the thief. If we can't, keep trying.
      if(!_atomic_cas_strong(&victim->thief, &thief, sched))
        continue;

      assert(sched->victim == NULL);
      sched->victim = victim;
    }

    // Record that this is our victim and return it.
    sched->last_victim = victim;
    return victim;
  }

  return NULL;
}

/**
 * Use mpmcqs to allow stealing directly from a victim, without waiting for a
 * response. However, this makes pushing and popping actors on scheduling
 * queues significantly more expensive.
 */
static pony_actor_t* steal(scheduler_t* sched)
{
  send_msg(0, SCHED_BLOCK, 0);
  uint64_t tsc = cpu_rdtsc();
  pony_actor_t* actor;

  while(true)
  {
    scheduler_t* victim = choose_victim(sched);

    if(victim == NULL)
      victim = sched;

    actor = pop_global(victim);

    if(actor != NULL)
      break;

    if(quiescent(sched, tsc))
      return NULL;

  }

  send_msg(0, SCHED_UNBLOCK, 0);
  return actor;
}

/**
 * Wait until we receive a stolen actor. Tight spin at first, falling back to
 * nanosleep. Once we have fallen back, check for quiescence.
 */
static pony_actor_t* request(scheduler_t* sched)
{
  send_msg(0, SCHED_BLOCK, 0);
  scheduler_t* thief = NULL;

  bool block = _atomic_cas_strong(&sched->thief, &thief, (void*)1);

  uint64_t tsc = cpu_rdtsc();
  pony_actor_t* actor;

  while(true)
  {
    _atomic_store(&sched->waiting, 1);

    scheduler_t* victim = choose_victim(sched);

    if(victim != NULL)
    {
      while(_atomic_load(&sched->waiting) == 1)
      {
        if(quiescent(sched, tsc))
          return NULL;
      }

      sched->victim = NULL;
    } else {
      if((actor = pop_global(sched)) != NULL)
      {
        _atomic_store(&sched->waiting, 0);
        break;
      }

      if(quiescent(sched, tsc))
        return NULL;
    }

    if((actor = pop(sched)) != NULL)
      break;
  }

  if(block)
  {
    thief = (scheduler_t*)1;

    /** MSVC++ throws warning here - non-void expression with no effect.
     *  GCC/clang would throw a warning for unused result, hence disable the
     *  MSVC++ warning.
     *
     *  http://msdn.microsoft.com/en-us/library/577cze47.aspx
     */
#if defined(PLATFORM_IS_WINDOWS)
#  pragma warning(disable:4552)
#endif
    _atomic_cas_strong(&sched->thief, &thief, NULL);
  }

  send_msg(0, SCHED_UNBLOCK, 0);
  return actor;
}

/**
 * Check if we have a thief. If we do, try to give it an actor. Signal the
 * thief to continue whether or not we gave it an actor.
 */
static void respond(scheduler_t* sched)
{
  scheduler_t* thief = _atomic_load(&sched->thief);

  if(thief <= (scheduler_t*)1)
    return;

  pony_actor_t* actor = pop_global(sched);

  if(actor != NULL)
  {
    assert(thief->waiting == 1);
    push(thief, actor);
  }

  assert(sched->thief == thief);
  _atomic_store(&thief->waiting, 0);
  _atomic_store(&sched->thief, NULL);
}

/**
 * Run a scheduler thread until termination.
 */
static void run(scheduler_t* sched)
{
  pony_actor_t* actor = pop_global(sched);

  while(true)
  {
    // if there are message to process, schedule task runner
    uint32_t counter = __atomic_load_n(&remaining_tasks, __ATOMIC_RELAXED);
    if(counter>0 && is_unscheduled((pony_actor_t*) this_encore_task)){
      unset_unscheduled((pony_actor_t*) this_encore_task);
      push(sched, (pony_actor_t*) this_encore_task);
    }

    assert(sched == this_scheduler);
    if(actor == NULL)
    {
      // We had an empty queue and no rescheduled actor.
      if(use_mpmcq)
        actor = steal(sched);
      else
        actor = request(sched);

      if(actor == NULL)
      {
        // Termination.
        assert(pop(sched) == NULL);
        return;
      }
    }

    // Respond to our thief. We hold an actor for ourself, to make sure we
    // never give away our last actor.
    if(!use_mpmcq)
      respond(sched);

    // Run the current actor and get the next actor.
    bool reschedule = actor_run(actor);
#ifdef LAZY_IMPL
    sched = this_scheduler;
#endif
    pony_actor_t* next = pop_global(sched);

    if(reschedule)
    {
#ifndef LAZY_IMPL
      actor_unlock((encore_actor_t*)actor);
#endif
      if(next != NULL)
      {
        // If we have a next actor, we go on the back of the queue. Otherwise,
        // we continue to run this actor.
        push(sched, actor);
        actor = next;
      }
    } else {
#ifndef LAZY_IMPL
      actor_unlock((encore_actor_t*)actor);
#endif
      // We aren't rescheduling, so run the next actor. This may be NULL if our
      // queue was empty.
      actor = next;
    }
  }
}

static void jump_buffer()
{
  __atomic_fetch_add(&thread_exit, 1, __ATOMIC_RELAXED);
  while(__atomic_load_n(&thread_exit, __ATOMIC_RELAXED) < scheduler_count) ;
}

static void __attribute__ ((noreturn)) jump_origin()
{
  static __pony_thread_local char stack[MINSIGSTKSZ];
  static __pony_thread_local ucontext_t ctx;
  getcontext(&ctx);
  ctx.uc_stack.ss_sp = stack;
  ctx.uc_stack.ss_size = MINSIGSTKSZ;
  ctx.uc_link = origin;
  makecontext(&ctx, (void(*)(void))&jump_buffer, 0);
  setcontext(&ctx);
  assert(0);
  exit(0);
}

__attribute__ ((noreturn))
void public_run(pony_actor_t *actor)
{
  assert(this_scheduler);
  if (pony_reschedule()) {
    push(this_scheduler, actor);
  }
  actor_unlock((encore_actor_t *)actor);
  run(this_scheduler);

  __atomic_fetch_add(&context_waiting, 1, __ATOMIC_RELAXED);
  while(__atomic_load_n(&context_waiting, __ATOMIC_RELAXED) < scheduler_count) ;

  jump_origin();
}


static void *run_thread(void *arg)
{
  scheduler_t* sched = (scheduler_t*) arg;
  this_scheduler = sched;
  cpu_affinity(sched->cpu);

#ifdef LAZY_IMPL
  context ctx;
  this_context = &ctx;
  root = &ctx.ctx;
  ucontext_t ucontext;
  origin = &ucontext;

  getcontext(origin);
  if (__atomic_load_n(&context_waiting, __ATOMIC_RELAXED) == scheduler_count) {
    return NULL;
  }
#endif

  // setup task runner
  assert(this_encore_task==NULL);
  this_encore_task = encore_create(task_gettype());
  scheduler_add((pony_actor_t*) this_encore_task);

  run(sched);

#ifdef LAZY_IMPL
  __atomic_fetch_add(&context_waiting, 1, __ATOMIC_RELAXED);
  while(__atomic_load_n(&context_waiting, __ATOMIC_RELAXED) < scheduler_count) ;

  jump_origin();
#endif

  return NULL;
}

static void scheduler_shutdown()
{
  uint32_t start;

  if(scheduler[0].tid == pony_thread_self())
    start = 1;
  else
    start = 0;

  for(uint32_t i = start; i < scheduler_count; i++) {
    pony_thread_join(scheduler[i].tid);
    assert(pop(&scheduler[i]) == NULL);
  }
  assert(pop(&scheduler[0]) == NULL);

  for(uint32_t i = 0; i < scheduler_count; i++)
  {
    while(messageq_pop(&scheduler[i].mq) != NULL);
    messageq_destroy(&scheduler[i].mq);
    mpmcq_destroy(&scheduler[i].q);
  }

  free(scheduler);
  scheduler = NULL;
  scheduler_count = 0;

  mpmcq_destroy(&inject);
}

void scheduler_init(uint32_t threads, bool noyield, bool forcecd, bool mpmcq)
{
  use_mpmcq = mpmcq;
  use_yield = !noyield;

  // If no thread count is specified, use the available physical core count.
  if(threads == 0)
    threads = cpu_count();

  scheduler_count = threads;
  scheduler = (scheduler_t*)calloc(scheduler_count, sizeof(scheduler_t));

  cpu_assign(scheduler_count, scheduler);

  scheduler[0].forcecd = forcecd;

  for(uint32_t i = 0; i < scheduler_count; i++)
  {
    scheduler[i].last_victim = &scheduler[i];
    messageq_init(&scheduler[i].mq);
    mpmcq_init(&scheduler[i].q);
  }

  mpmcq_init(&inject);
  asio_init();
}

bool scheduler_start(bool library)
{
  if(!asio_start())
    return false;

  detect_quiescence = !library;

  uint32_t start;

  if(library)
  {
    start = 0;
  } else {
    start = 1;
    scheduler[0].tid = pony_thread_self();
  }

  for(uint32_t i = start; i < scheduler_count; i++)
  {
    if(!pony_thread_create(&scheduler[i].tid, run_thread, scheduler[i].cpu,
      &scheduler[i]))
      return false;
  }

  if(!library)
  {
    run_thread(&scheduler[0]);
    scheduler_shutdown();
  }

  return true;
}

void scheduler_stop()
{
  _atomic_store(&detect_quiescence, true);
  scheduler_shutdown();
}

pony_actor_t* scheduler_worksteal()
{
  // TODO: is this right?
  return pop_global(this_scheduler);
}

void scheduler_add_first(pony_actor_t* actor)
{
  assert(this_scheduler);
  push_first(this_scheduler, actor);
}

void scheduler_add(pony_actor_t* actor)
{
  if(this_scheduler != NULL)
  {
    // Add to the current scheduler thread.
    push(this_scheduler, actor);
  } else {
    // Put on the shared mpmcq.
    mpmcq_push(&inject, actor);
  }
}

void scheduler_respond()
{
  // Check for a pending thief.
  if(!use_mpmcq && (this_scheduler != NULL))
    respond(this_scheduler);
}

void scheduler_offload()
{
  scheduler_t* sched = this_scheduler;

  if(sched == NULL)
    return;

  pony_actor_t* actor;

  while((actor = pop(sched)) != NULL)
    mpmcq_push(&inject, actor);
}

void scheduler_terminate()
{
  for(uint32_t i = 0; i < scheduler_count; i++)
    send_msg(i, SCHED_TERMINATE, 0);
}

uint32_t scheduler_cores()
{
  return scheduler_count;
}
