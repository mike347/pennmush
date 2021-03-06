/**
 * \file timer.c
 *
 * \brief Timed events in PennMUSH.
 *
 *
 */

#include "copyrite.h"

#include <stdio.h>
#include <ctype.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#ifdef TIME_WITH_SYS_TIME
#include <time.h>
#endif
#else
#include <time.h>
#endif
#ifdef WIN32
#include <windows.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "access.h"
#include "attrib.h"
#include "conf.h"
#include "dbdefs.h"
#include "externs.h"
#include "extmail.h"
#include "flags.h"
#include "game.h"
#include "help.h"
#include "lock.h"
#include "log.h"
#include "match.h"
#include "memcheck.h"
#include "mymalloc.h"
#include "parse.h"
#include "sig.h"
#include "strutil.h"

bool inactivity_check(void);
static void migrate_stuff(int amount);

#ifndef WIN32
void hup_handler(int);
void usr1_handler(int);

#endif
void dispatch(void);

/** Set up signal handlers.
 */
void
init_timer(void)
{
#ifndef PROFILING
#ifdef HAVE_SETITIMER
  install_sig_handler(SIGALRM, signal_cpu_limit);
  install_sig_handler(SIGPROF, signal_cpu_limit);
#endif
#endif
}

/** Migrate some number of chunks.
 * The requested amount is only a guideline; the actual amount
 * migrated will be more or less due to always migrating all the
 * attributes, locks, and mail on any given object together.
 * \param amount the suggested number of attributes to migrate.
 */
static void
migrate_stuff(int amount)
{
  static int start_obj = 0;
  static chunk_reference_t **refs = NULL;
  static int refs_size = 0;
  int end_obj;
  int actual;
  ATTR *aptr;
  lock_list *lptr;
  MAIL *mp;

  if (db_top == 0)
    return;

  end_obj = start_obj;
  actual = 0;
  do {
    ATTR_FOR_EACH (end_obj, aptr) {
      if (aptr->data != NULL_CHUNK_REFERENCE)
        actual++;
    }
    for (lptr = Locks(end_obj); lptr; lptr = L_NEXT(lptr))
      if (L_KEY(lptr) != NULL_CHUNK_REFERENCE)
        actual++;
    if (IsPlayer(end_obj)) {
      for (mp = find_exact_starting_point(end_obj); mp; mp = mp->next)
        if (mp->msgid != NULL_CHUNK_REFERENCE)
          actual++;
    }
    end_obj = (end_obj + 1) % db_top;
  } while (actual < amount && end_obj != start_obj);

  if (actual == 0)
    return;

  if (!refs || actual > refs_size) {
    if (refs)
      mush_free(refs, "migration reference array");
    refs = mush_calloc(actual, sizeof(chunk_reference_t *),
                       "migration reference array");
    refs_size = actual;
    if (!refs)
      mush_panic("Could not allocate migration reference array");
  }
#ifdef DEBUG_MIGRATE
  do_rawlog(LT_TRACE, "Migrate asked %d, actual objects #%d to #%d for %d",
            amount, start_obj, (end_obj + db_top - 1) % db_top, actual);
#endif

  actual = 0;
  do {
    ATTR_FOR_EACH (start_obj, aptr) {
      if (aptr->data != NULL_CHUNK_REFERENCE) {
        refs[actual] = &(aptr->data);
        actual++;
      }
    }
    for (lptr = Locks(start_obj); lptr; lptr = L_NEXT(lptr))
      if (L_KEY(lptr) != NULL_CHUNK_REFERENCE) {
        refs[actual] = &(lptr->key);
        actual++;
      }
    if (IsPlayer(start_obj)) {
      for (mp = find_exact_starting_point(start_obj); mp; mp = mp->next)
        if (mp->msgid != NULL_CHUNK_REFERENCE) {
          refs[actual] = &(mp->msgid);
          actual++;
        }
    }
    start_obj = (start_obj + 1) % db_top;
  } while (start_obj != end_obj);

  chunk_migration(actual, refs);
}

static bool
idle_event(void *data __attribute__((__unused__)))
{
  return inactivity_check();
}

static bool
purge_event(void *data __attribute__((__unused__)))
{
  if (PURGE_INTERVAL <= 0)
    return false; /* in case purge_interval is set to 0 with @config */
  purge();
  options.purge_counter = mudtime + PURGE_INTERVAL;
  sq_register_in(PURGE_INTERVAL, purge_event, NULL, "DB`PURGE");
  return true;
}

static bool
dbck_event(void *data __attribute__((__unused__)))
{
  if (DBCK_INTERVAL <= 0)
    return false; /* in case dbck_interval is set to 0 with @config */
  dbck();
  options.dbck_counter = mudtime + DBCK_INTERVAL;
  sq_register_in(DBCK_INTERVAL, dbck_event, NULL, "DB`DBCK");
  return true;
}

static bool
warning_event(void *data __attribute__((__unused__)))
{
  if (options.warn_interval <= 0)
    return false; /* in case warn_interval is set to 0 with @config */
  options.warn_counter = options.warn_interval + mudtime;
  run_topology();
  sq_register_in(options.warn_interval, warning_event, NULL, "DB`WCHECK");
  return true;
}

/** Info on the events run for impending dbsaves */
struct dbsave_warn_data {
  int secs;          /**< How many seconds before the dbsave to run */
  const char *event; /**< The name of the event to trigger */
  char *msg;         /**< The \@config'd message to show */
};

struct dbsave_warn_data dbsave_5min = {300, "DUMP`5MIN",
                                       options.dump_warning_5min};
struct dbsave_warn_data dbsave_1min = {60, "DUMP`1MIN",
                                       options.dump_warning_1min};

static bool
dbsave_warn_event(void *data)
{
  struct dbsave_warn_data *when = data;

  queue_event(SYSEVENT, when->event, "%s,%d", when->msg, NO_FORK ? 0 : 1);
  if (NO_FORK && *(when->msg))
    flag_broadcast(0, 0, "%s", when->msg);
  return false;
}

static void
reg_dbsave_warnings(void)
{
  if (DUMP_INTERVAL > dbsave_5min.secs)
    sq_register_in(DUMP_INTERVAL - dbsave_5min.secs, dbsave_warn_event,
                   &dbsave_5min, NULL);
  if (DUMP_INTERVAL > dbsave_1min.secs)
    sq_register_in(DUMP_INTERVAL - dbsave_1min.secs, dbsave_warn_event,
                   &dbsave_1min, NULL);
}

static bool
dbsave_event(void *data __attribute__((__unused__)))
{
  if (options.dump_interval <= 0)
    return false; /* in case dump_interval is set to 0 with @config */

  log_mem_check();
  options.dump_counter = options.dump_interval + mudtime;
  fork_and_dump(1);
  flag_broadcast(0, "ON-VACATION", "%s",
                 T("Your ON-VACATION flag is set! If you're back, clear it."));
  reg_dbsave_warnings();
  sq_register_in(DUMP_INTERVAL, dbsave_event, NULL, NULL);
  return false;
}

static bool
migrate_event(void *data __attribute__((__unused__)))
{
  migrate_stuff(CHUNK_MIGRATE_AMOUNT);
  return false;
}

static bool
on_every_second(void *data __attribute__((__unused__)))
{
  time(&mudtime);
  do_second();
  return false;
}

void
init_sys_events(void)
{
  time(&mudtime);
  sq_register_loop(60, idle_event, NULL, "PLAYER`INACTIVITY");
  if (DBCK_INTERVAL > 0) {
    sq_register(mudtime + DBCK_INTERVAL, dbck_event, NULL, "DB`DBCK");
    options.dbck_counter = mudtime + DBCK_INTERVAL;
  }
  if (PURGE_INTERVAL > 0) {
    sq_register(mudtime + PURGE_INTERVAL, purge_event, NULL, "DB`PURGE");
    options.purge_counter = mudtime + PURGE_INTERVAL;
  }
  if (options.warn_interval > 0) {
    sq_register(mudtime + options.warn_interval, warning_event, NULL,
                "DB`WCHECK");
    options.warn_counter = mudtime + options.warn_interval;
  }
  reg_dbsave_warnings();
  if (DUMP_INTERVAL > 0) {
    sq_register(mudtime + DUMP_INTERVAL, dbsave_event, NULL, NULL);
    options.dump_counter = mudtime + DUMP_INTERVAL;
  }
  /* The chunk migration normally runs every 1 second. Slow it down a bit
     to see what affect it has on CPU time */
  sq_register_loop(20, migrate_event, NULL, NULL);
  sq_register_loop(1, on_every_second, NULL, NULL);
}

volatile sig_atomic_t cpu_time_limit_hit = 0; /** Was the cpu time limit hit? */
int cpu_limit_warning_sent = 0; /** Have we issued a cpu limit warning? */

#ifndef PROFILING
#if defined(HAVE_SETITIMER)
/** Handler for PROF signal.
 * Do the minimal work here - set a global variable and reload the handler.
 * \param signo unused.
 */
void
signal_cpu_limit(int signo)
{
  cpu_time_limit_hit = 1;
  reload_sig_handler(signo, signal_cpu_limit);
}

#elif defined(WIN32)
#if _MSC_VER <= 1100 && !defined(UINT_PTR)
#define UINT_PTR UINT
#endif
UINT_PTR timer_id;
VOID CALLBACK
win32_timer(HWND hwnd __attribute__((__unused__)),
            UINT uMsg __attribute__((__unused__)),
            UINT_PTR idEvent __attribute__((__unused__)),
            DWORD dwTime __attribute__((__unused__)))
{
  cpu_time_limit_hit = 1;
}
#endif
#endif
int timer_set = 0; /**< Is a CPU timer set? */

/* setitmer() supports multiple types of timer clocks. Windows-based
 * environments (Ubuntu For Windows, Cygwin, etc.) only support
 * ITIMER_REAL, which isn't the most useful. Fall back on that if
 * ITIMER_PROF fails. */
#ifdef HAVE_SETITIMER
static int itimer_which = ITIMER_PROF;
#endif

/** Start the cpu timer (before running a command).
 */
void
start_cpu_timer(void)
{
#ifndef PROFILING
  cpu_time_limit_hit = 0;
  cpu_limit_warning_sent = 0;
  timer_set = 1;
#if defined(HAVE_SETITIMER) /* UNIX way */
  {
    struct itimerval time_limit;
    if (options.queue_entry_cpu_time > 0) {
      ldiv_t t;
      /* Convert from milliseconds */
      t = ldiv(options.queue_entry_cpu_time, 1000);
      time_limit.it_value.tv_sec = t.quot;
      time_limit.it_value.tv_usec = t.rem * 1000;
      time_limit.it_interval.tv_sec = 0;
      time_limit.it_interval.tv_usec = 0;
      if (setitimer(itimer_which, &time_limit, NULL)) {
        if (itimer_which == ITIMER_PROF) {
          itimer_which = ITIMER_REAL;
          start_cpu_timer();
          return;
        } else {
          penn_perror("setitimer");
          timer_set = 0;
        }
      }
    } else
      timer_set = 0;
  }
#elif defined(WIN32) /* Windoze way */
  if (options.queue_entry_cpu_time > 0)
    timer_id = SetTimer(NULL, 0, (unsigned) options.queue_entry_cpu_time,
                        (TIMERPROC) win32_timer);
  else
    timer_set = 0;
#endif               /* HAVE_SETITIMER / WIN32 */
#endif               /* PROFILING */
}

/** Reset the cpu timer (after running a command).
 */
void
reset_cpu_timer(void)
{
#ifndef PROFILING
  if (timer_set) {
#if defined(HAVE_SETITIMER)
    struct itimerval time_limit, time_left;
    time_limit.it_value.tv_sec = 0;
    time_limit.it_value.tv_usec = 0;
    time_limit.it_interval.tv_sec = 0;
    time_limit.it_interval.tv_usec = 0;
    if (setitimer(itimer_which, &time_limit, &time_left))
      penn_perror("setitimer");
#elif defined(WIN32)
    KillTimer(NULL, timer_id);
#endif /* HAVE_SETITIMER / WIN32 */
  }
  cpu_time_limit_hit = 0;
  cpu_limit_warning_sent = 0;
  timer_set = 0;
#endif /* PROFILING */
}

/** System queue stuff. Timed events like dbcks and purges are handled
 *  through this system. */

struct squeue *sq_head = NULL;

/** Register a callback function to be executed at a certain time.
 * \param w when to run the event
 * \param f the callback function
 * \param d data to pass to the callback
 * \param ev Softcode event to trigger at the same time.
 * \return pointer to the newly added squeue
 */
struct squeue *
sq_register(time_t w, sq_func f, void *d, const char *ev)
{
  struct squeue *sq;

  sq = mush_malloc(sizeof *sq, "squeue.node");

  sq->when = w;
  sq->fun = f;
  sq->data = d;
  if (ev)
    sq->event = strupper_a(ev, "squeue.event");
  else
    sq->event = NULL;
  sq->next = NULL;

  if (!sq_head)
    sq_head = sq;
  else if (difftime(w, sq_head->when) <= 0) {
    sq->next = sq_head;
    sq_head = sq;
  } else {
    struct squeue *c, *prev = NULL;
    for (prev = sq_head, c = sq_head->next; c; prev = c, c = c->next) {
      if (difftime(w, c->when) <= 0) {
        sq->next = c;
        prev->next = sq;
        return sq;
      }
    }
    prev->next = sq;
  }

  return sq;
}

/** Cancel an entry in the system queue.
 * \param sq systen queue entry to cancel
 */
void
sq_cancel(struct squeue *sq)
{
  struct squeue *tmp, *prev = NULL;

  if (!sq)
    return;

  /* Remove from list */
  for (tmp = sq_head; tmp; tmp = tmp->next) {
    if (tmp == sq) {
      if (prev)
        prev->next = tmp->next;
      else
        sq_head = tmp->next;
      if (sq->event)
        mush_free(sq->event, "squeue.event");
      mush_free(sq, "squeue.node");
      return;
    }
    prev = tmp;
  }
}

/** Register a callback function to be executed in N seconds.
 * \param n the number of seconds to run the callback after.
 * \param f the callback function.
 * \param d data to pass to the callback.
 * \param ev softcode event to trigger at the same time.
 * \return pointer to the newly added squeue
 */
struct squeue *
sq_register_in(int n, sq_func f, void *d, const char *ev)
{
  time_t now;
  time(&now);
  return sq_register(now + n, f, d, ev);
}

/** A timed event that runs on a loop */
struct sq_loop {
  sq_func fun;       /**< The function to run for the event */
  void *data;        /**< The data for the event */
  const char *event; /**< The name of the event attr to trigger */
  int secs;          /**< How often to run the event */
};

static bool
sq_loop_fun(void *arg)
{
  struct sq_loop *loop = arg;
  bool res;

  res = loop->fun(loop->data);
  sq_register_in(loop->secs, sq_loop_fun, arg, loop->event);

  return res;
}

/** Register a callback function to run every N seconds.
 * \param n the number of seconds to wait between calls.
 * \param f the callback function.
 * \param d data to pass to the callback.
 * \param ev softcode event to trigger at the same time.
 */
void
sq_register_loop(int n, sq_func f, void *d, const char *ev)
{
  struct sq_loop *loop;

  loop = mush_malloc(sizeof *loop, "squeue.node");
  loop->fun = f;
  loop->data = d;
  if (ev)
    loop->event = strupper_a(ev, "squeue.event");
  else
    loop->event = NULL;
  loop->secs = n;

  sq_register_in(n, sq_loop_fun, loop, ev);
}

/** Execute a single pending system queue event.
 * \return true if work was done, false otherwise.
 */
bool
sq_run_one(void)
{
  time_t now;
  struct squeue *n;

  time(&now);

  if (sq_head) {
    if (difftime(sq_head->when, now) <= 0) {
      bool r = sq_head->fun(sq_head->data);
      if (r && sq_head->event)
        queue_event(SYSEVENT, sq_head->event, "%s", "");
      n = sq_head->next;
      if (sq_head->event)
        mush_free(sq_head->event, "squeue.event");
      mush_free(sq_head, "squeue.node");
      sq_head = n;
      return true;
    }
  }
  return false;
}

/** Run all pending system queue events.
 * \return true if work was done, false otherwise.
 */
bool
sq_run_all(void)
{
  bool r, any = false;

  do {
    r = sq_run_one();
    if (r)
      any = true;
  } while (r);
  return any;
}

int
sq_secs_till_next(void)
{
  time_t now = time(NULL);
  for (struct squeue *s = sq_head; s; s = s->next) {
    if (s->fun == sq_loop_fun &&
        ((struct sq_loop *) s->data)->fun == on_every_second)
      continue;
    return difftime(s->when, now);
  }
  return 500;
}
