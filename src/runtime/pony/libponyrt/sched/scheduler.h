#ifndef sched_scheduler_h
#define sched_scheduler_h

#include <pony.h>
// #include <platform.h>
#include "actor/messageq.h"
#include "mpmcq.h"

PONY_EXTERN_C_BEGIN

typedef struct scheduler_t
{
  // These are rarely changed.
  pony_thread_id_t tid;
  uint32_t cpu;
  uint32_t node;
  bool forcecd;
  bool terminate;
  bool asio_stopped;

  // These are changed primarily by the owning scheduler thread.
  __pony_spec_align__(struct scheduler_t* last_victim, 64);
  pony_actor_t* head;
  pony_actor_t* tail;
  struct scheduler_t* victim;

  uint32_t block_count;
  uint32_t ack_token;
  uint32_t ack_count;

  // These are accessed by other scheduler threads.
  __pony_spec_align__(struct scheduler_t* volatile thief, 64);
  uint32_t volatile waiting;

  messageq_t mq;
  mpmcq_t q;
} scheduler_t;

void scheduler_init(uint32_t threads, bool noyield, bool forcecd, bool mpmcq);

bool scheduler_start(bool library);

void scheduler_stop();

pony_actor_t* scheduler_worksteal();

void scheduler_add(pony_actor_t* actor);

void scheduler_add_first(pony_actor_t* actor);

void scheduler_respond();

void scheduler_offload();

uint32_t scheduler_cores();

void scheduler_terminate();

PONY_EXTERN_C_END

#endif
