#include "scheduler.h"
#include "../gc/cycle.h"
#include "../dist/dist.h"
#include <string.h>
#include <stdlib.h>

typedef struct options_t
{
  // concurrent options
  uint32_t threads;

  // distributed options
  bool distrib;
  bool master;
  uint32_t child_count;
  char* port;
  char* parent_host;
  char* parent_port;

  // debugging options
  bool forcecd;
} options_t;

// global data
static int exit_code;

static int parse_opts(int argc, char** argv, options_t* opt)
{
  int remove = 0;

  for(int i = 0; i < argc; i++)
  {
    if(!strcmp(argv[i], "--ponythreads"))
    {
      remove++;

      if(i < (argc - 1))
      {
        opt->threads = atoi(argv[i + 1]);
        remove++;
      }
    } else if(!strcmp(argv[i], "--ponyforcecd")) {
      remove++;
      opt->forcecd = true;
    } else if(!strcmp(argv[i], "--ponydistrib")) {
      remove++;
      opt->distrib = true;
    } else if(!strcmp(argv[i], "--ponymaster")) {
      remove++;
      opt->master = true;
    } else if(!strcmp(argv[i], "--ponylisten")) {
      remove++;

      if(i < (argc - 1))
      {
        opt->port = argv[i + 1];
        remove++;
      }
    } else if(!strcmp(argv[i], "--ponychildren")) {
      remove++;

      if(i < (argc - 1))
      {
        opt->child_count = atoi(argv[i + 1]);
        remove++;
      }
    } else if(!strcmp(argv[i], "--ponyconnect")) {
      remove++;

      if(i < (argc - 2))
      {
        opt->parent_host = argv[i + 1];
        opt->parent_port = argv[i + 1];
        remove += 2;
      }
    }

    if(remove > 0)
    {
      argc -= remove;
      memmove(&argv[i], &argv[i + remove], (argc - i) * sizeof(char*));
      remove = 0;
      i--;
    }
  }

  return argc;
}

static int setup(int argc, char** argv)
{
  options_t opt;
  memset(&opt, 0, sizeof(options_t));
  argc = parse_opts(argc, argv, &opt);

  pony_exitcode(0);
  scheduler_init(opt.threads, opt.forcecd);
  cycle_create();

  if(opt.distrib)
  {
    dist_create(opt.port, opt.child_count, opt.master);

    if(!opt.master)
      dist_join(opt.parent_host, opt.parent_port);
  }

  return argc;
}

int pony_start(int argc, char** argv, pony_actor_t* actor)
{
  argc = setup(argc, argv);

  //the program is started on the master node.
  //if(!distrib || master)
  //{
    pony_arg_t arg[2];
    arg[0].i = argc;
    arg[1].p = argv;
    pony_sendv(actor, PONY_MAIN, 2, arg);
  //}

  if(!scheduler_run(false))
    return -1;

  scheduler_finish();
  return __atomic_load_n(&exit_code, __ATOMIC_ACQUIRE);
}

int pony_init(int argc, char** argv)
{
  argc = setup(argc, argv);

  if(!scheduler_run(true))
    return -1;

  return argc;
}

int pony_shutdown()
{
  scheduler_finish();
  return __atomic_load_n(&exit_code, __ATOMIC_ACQUIRE);
}

void pony_exitcode(int code)
{
  __atomic_store_n(&exit_code, code, __ATOMIC_RELEASE);
}
