/*
 * Copyright 2026
 * SPDX-License-Identifier: MIT
 */

#include "proxy_monitor.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/wait.h>
#include "c11/threads.h"
#include <unistd.h>

struct proxy_monitor_watch {
   pid_t pid;
   proxy_monitor_exit_fn cb;
   void *udata;
   bool registered; /* EVFILT_PROC knote still installed */
   bool fired;      /* exit already dispatched */
   bool unwatched;
   struct proxy_monitor_watch *next;
};

/* monitor_mtx serializes registration, dispatch and unwatch.  A watch
 * is only ever freed by the monitor thread: kevent may already have
 * returned an event referencing it while the thread waits on the
 * mutex, so unwatch moves watches onto the retired list instead of
 * freeing, and the thread sweeps that list under the mutex after each
 * batch.  An EVFILT_USER trigger pokes the thread so the sweep is not
 * deferred until the next exit.
 *
 * A fired watch stays allocated until its owner unwatches it: callers
 * always unwatch at destroy, whether or not the exit already ran. */
static struct {
   int kq;
   thrd_t thread;
   bool started;
   mtx_t mtx;
   struct proxy_monitor_watch *watches; /* registered */
   struct proxy_monitor_watch *dead;    /* fired, awaiting unwatch */
   struct proxy_monitor_watch *retired; /* unwatched, awaiting free */
} monitor = {
   .kq = -1,
};

#define PROXY_MONITOR_USER_IDENT 1

static void
proxy_monitor_log_exit(pid_t pid, int code, int status)
{
   if (code == CLD_KILLED || code == CLD_DUMPED)
      fprintf(stderr, "proxy_monitor: worker %d died on signal %d%s\n", pid,
              status, code == CLD_DUMPED ? " (core dumped)" : "");
   else if (code == CLD_EXITED && status != 0)
      fprintf(stderr, "proxy_monitor: worker %d exited with status %d\n", pid,
              status);
}

/* caller holds monitor.mtx; removes watch from whichever list it is on */
static void
proxy_monitor_unlink_locked(struct proxy_monitor_watch *watch)
{
   struct proxy_monitor_watch *lists[] = { monitor.watches, monitor.dead,
                                           monitor.retired };
   for (int l = 0; l < 3; l++) {
      struct proxy_monitor_watch **it = &lists[l];
      while (*it) {
         if (*it == watch) {
            *it = watch->next;
            break;
         }
         it = &(*it)->next;
      }
   }
   /* write back: lists[] held copies of the heads */
   monitor.watches = lists[0];
   monitor.dead = lists[1];
   monitor.retired = lists[2];
}

/* caller holds monitor.mtx */
static void
proxy_monitor_sweep_locked(void)
{
   struct proxy_monitor_watch *retired = monitor.retired;
   monitor.retired = NULL;
   while (retired) {
      struct proxy_monitor_watch *next = retired->next;
      free(retired);
      retired = next;
   }
}

static void
proxy_monitor_poke(void)
{
   struct kevent trigger;
   EV_SET(&trigger, PROXY_MONITOR_USER_IDENT, EVFILT_USER, 0, NOTE_TRIGGER,
          0, NULL);
   kevent(monitor.kq, &trigger, 1, NULL, 0, NULL);
}

static int
proxy_monitor_thread(void *arg)
{
   (void)arg;

   for (;;) {
      struct kevent evs[8];
      const int n = kevent(monitor.kq, NULL, 0, evs, 8, NULL);
      if (n < 0) {
         if (errno == EINTR)
            continue;
         break; /* kq closed by shutdown */
      }

      mtx_lock(&monitor.mtx);
      for (int i = 0; i < n; i++) {
         if (evs[i].filter == EVFILT_USER)
            continue;
         struct proxy_monitor_watch *watch = evs[i].udata;
         /* a stale event for a since-unwatched pid finds registered
          * false; the memory is still valid because only this thread
          * frees watches */
         if (!watch || !watch->registered)
            continue;

         struct kevent kev;
         EV_SET(&kev, watch->pid, EVFILT_PROC, EV_DELETE, 0, 0, NULL);
         kevent(monitor.kq, &kev, 1, NULL, 0, NULL);
         watch->registered = false;
         watch->fired = true;
         proxy_monitor_unlink_locked(watch);
         watch->next = monitor.dead;
         monitor.dead = watch;

         bool reaped = false;
         siginfo_t siginfo = { 0 };
         if (!waitid(P_PID, (id_t)watch->pid, &siginfo,
                     WEXITED | WNOHANG) &&
             siginfo.si_pid == watch->pid) {
            reaped = true;
            proxy_monitor_log_exit(watch->pid, siginfo.si_code,
                                   siginfo.si_status);
         }
         if (watch->cb)
            watch->cb(watch->udata, reaped);
      }
      /* every evs[] entry was inspected above; retired watches can no
       * longer be reached from this or any future batch */
      proxy_monitor_sweep_locked();
      mtx_unlock(&monitor.mtx);
   }

   return 0;
}

static once_flag proxy_monitor_once = ONCE_FLAG_INIT;
static bool proxy_monitor_start_ok;

static void
proxy_monitor_start_once(void)
{
   if (mtx_init(&monitor.mtx, mtx_plain) != thrd_success)
      return;
   monitor.watches = NULL;
   monitor.dead = NULL;
   monitor.retired = NULL;

   monitor.kq = kqueue();
   if (monitor.kq < 0) {
      fprintf(stderr, "proxy_monitor: kqueue failed: %s\n", strerror(errno));
      mtx_destroy(&monitor.mtx);
      return;
   }
   struct kevent kev;
   EV_SET(&kev, PROXY_MONITOR_USER_IDENT, EVFILT_USER, EV_ADD | EV_CLEAR, 0,
          0, NULL);
   if (kevent(monitor.kq, &kev, 1, NULL, 0, NULL)) {
      close(monitor.kq);
      monitor.kq = -1;
      mtx_destroy(&monitor.mtx);
      return;
   }
   if (thrd_create(&monitor.thread, proxy_monitor_thread, NULL) !=
       thrd_success) {
      fprintf(stderr, "proxy_monitor: thread create failed\n");
      close(monitor.kq);
      monitor.kq = -1;
      mtx_destroy(&monitor.mtx);
      return;
   }
   monitor.started = true;
   proxy_monitor_start_ok = true;
}

bool
proxy_monitor_start(void)
{
   call_once(&proxy_monitor_once, proxy_monitor_start_once);
   return proxy_monitor_start_ok;
}

void
proxy_monitor_shutdown(void)
{
   if (!monitor.started)
      return;
   /* closing the kqueue makes the monitor thread's kevent fail and
    * exit; only safe once no watches remain */
   close(monitor.kq);
   monitor.kq = -1;
   thrd_join(monitor.thread, NULL);

   struct proxy_monitor_watch *lists[] = { monitor.watches, monitor.dead,
                                           monitor.retired };
   for (int l = 0; l < 3; l++) {
      while (lists[l]) {
         struct proxy_monitor_watch *next = lists[l]->next;
         free(lists[l]);
         lists[l] = next;
      }
   }
   monitor.watches = monitor.dead = monitor.retired = NULL;
   mtx_destroy(&monitor.mtx);
   monitor.started = false;
}

struct proxy_monitor_watch *
proxy_monitor_watch(pid_t pid, proxy_monitor_exit_fn cb, void *udata)
{
   struct proxy_monitor_watch *watch = calloc(1, sizeof(*watch));
   if (!watch)
      return NULL;
   watch->pid = pid;
   watch->cb = cb;
   watch->udata = udata;

   mtx_lock(&monitor.mtx);
   struct kevent kev;
   EV_SET(&kev, pid, EVFILT_PROC, EV_ADD | EV_ENABLE, NOTE_EXIT, 0, watch);
   if (kevent(monitor.kq, &kev, 1, NULL, 0, NULL)) {
      mtx_unlock(&monitor.mtx);
      fprintf(stderr, "proxy_monitor: watch %d failed: %s\n", (int)pid,
              strerror(errno));
      free(watch);
      return NULL;
   }
   watch->registered = true;
   watch->next = monitor.watches;
   monitor.watches = watch;
   mtx_unlock(&monitor.mtx);

   return watch;
}

void
proxy_monitor_unwatch(struct proxy_monitor_watch *watch)
{
   if (!watch)
      return;

   /* after this call returns no monitor callback can still touch this
    * watch: the monitor holds the same mutex while dispatching, and a
    * queued stale event finds registered == false */
   mtx_lock(&monitor.mtx);
   if (watch->registered) {
      struct kevent kev;
      EV_SET(&kev, watch->pid, EVFILT_PROC, EV_DELETE, 0, 0, NULL);
      kevent(monitor.kq, &kev, 1, NULL, 0, NULL);
      watch->registered = false;
   }
   proxy_monitor_unlink_locked(watch);
   watch->unwatched = true;
   watch->next = monitor.retired;
   monitor.retired = watch;
   mtx_unlock(&monitor.mtx);

   proxy_monitor_poke();
}
