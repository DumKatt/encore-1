#ifndef ds_stack_h
#define ds_stack_h

#include <platform.h>

PONY_EXTERN_C_BEGIN

typedef struct Stack Stack;

Stack* stack_pop(Stack* stack, void** data);

Stack* stack_push(Stack* list, void* data);

#define DECLARE_STACK(name, elem) \
  typedef struct name##_t name##_t; \
  name##_t* name##_pop(name##_t* stack, elem** data); \
  name##_t* name##_push(name##_t* stack, elem* data); \

#define DEFINE_STACK(name, elem) \
  struct name##_t {}; \
  \
  name##_t* name##_pop(name##_t* stack, elem** data) \
  { \
    return (name##_t*)stack_pop((Stack*)stack, (void**)data); \
  } \
  name##_t* name##_push(name##_t* stack, elem* data) \
  { \
    return (name##_t*)stack_push((Stack*)stack, data); \
  } \

PONY_EXTERN_C_END

#endif
