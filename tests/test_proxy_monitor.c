/*
 * Copyright 2026
 * SPDX-License-Identifier: MIT
 */

/* Tests for the process-wide proxy child monitor
 * (src/proxy/proxy_monitor.c): a watched child that is SIGKILLed must
 * fire its exit callback promptly and be reaped; unwatch must be safe
 * both after and before the exit. */

#include "proxy/proxy_monitor.h"

#include <errno.h>
#include <pthread.h>
#include <spawn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;

#define CHECK(cond, ...)                                                       \
   do {                                                                        \
      if (!(cond)) {                                                           \
         fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);                  \
         fprintf(stderr, __VA_ARGS__);                                         \
         fprintf(stderr, "\n");                                                \
         failures++;                                                           \
      }                                                                        \
   } while (0)

struct cb_ctx {
   pthread_mutex_t mtx;
   pthread_cond_t cond;
   int fired;
   int reaped;
};

static void
cb(void *udata, bool reaped)
{
   struct cb_ctx *c = udata;
   pthread_mutex_lock(&c->mtx);
   c->fired = 1;
   c->reaped = reaped;
   pthread_cond_signal(&c->cond);
   pthread_mutex_unlock(&c->mtx);
}

static void
cb_ctx_init(struct cb_ctx *c)
{
   pthread_mutex_init(&c->mtx, NULL);
   pthread_cond_init(&c->cond, NULL);
   c->fired = 0;
   c->reaped = -1;
}

/* returns true if the callback fired within ms */
static bool
cb_ctx_wait(struct cb_ctx *c, int ms)
{
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   ts.tv_nsec += ms * 1000000l;
   ts.tv_sec += ts.tv_nsec / 1000000000l;
   ts.tv_nsec %= 1000000000l;

   pthread_mutex_lock(&c->mtx);
   while (!c->fired &&
          pthread_cond_timedwait(&c->cond, &c->mtx, &ts) != ETIMEDOUT) {
   }
   const bool fired = c->fired;
   pthread_mutex_unlock(&c->mtx);
   return fired;
}

static pid_t
spawn_child(void)
{
   extern char **environ;
   char *const argv[] = { (char *)"/bin/sleep", (char *)"60", NULL };
   pid_t pid = -1;
   if (posix_spawn(&pid, "/bin/sleep", NULL, NULL, argv, environ))
      return -1;
   return pid;
}

/* exit -> callback within 200 ms -> child reaped -> unwatch safe */
static void
test_exit_then_destroy(void)
{
   struct cb_ctx c;
   cb_ctx_init(&c);

   const pid_t pid = spawn_child();
   CHECK(pid > 0, "fork failed");
   struct proxy_monitor_watch *w = proxy_monitor_watch(pid, cb, &c);
   CHECK(w != NULL, "watch failed");

   kill(pid, SIGKILL);
   CHECK(cb_ctx_wait(&c, 200), "exit callback did not fire within 200 ms");
   CHECK(c.reaped == 1, "callback did not report reaped (got %d)",
         c.reaped);

   /* the monitor reaps via waitid: the child must not exist as a
    * zombie for us */
   const pid_t wr = waitpid(pid, NULL, WNOHANG);
   CHECK(wr == -1 && errno == ECHILD,
         "child not reaped by monitor (waitpid=%d errno=%d)", (int)wr,
         errno);

   /* destroy-after-exit */
   proxy_monitor_unwatch(w);
}

/* unwatch before exit: no callback may fire, and we reap ourselves */
static void
test_destroy_before_exit(void)
{
   struct cb_ctx c;
   cb_ctx_init(&c);

   const pid_t pid = spawn_child();
   CHECK(pid > 0, "fork failed");
   struct proxy_monitor_watch *w = proxy_monitor_watch(pid, cb, &c);
   CHECK(w != NULL, "watch failed");

   proxy_monitor_unwatch(w);

   kill(pid, SIGKILL);
   CHECK(!cb_ctx_wait(&c, 300), "callback fired for an unwatched child");

   int status = 0;
   CHECK(waitpid(pid, &status, 0) == pid, "manual reap failed");
   CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
         "unexpected child status %d", status);
}

/* watching an already-dead child fails with ESRCH -- the documented
 * contract the caller relies on (a null watch is a startup failure) */
static void
test_exit_before_watch(void)
{
   struct cb_ctx c;
   cb_ctx_init(&c);

   extern char **environ;
   char *const argv[] = { (char *)"/bin/sleep", (char *)"0", NULL };
   pid_t pid = -1;
   CHECK(posix_spawn(&pid, "/bin/sleep", NULL, NULL, argv, environ) == 0,
         "spawn failed");
   /* give the child a moment to die */
   const struct timespec d = { .tv_sec = 0, .tv_nsec = 50000000 };
   nanosleep(&d, NULL);

   struct proxy_monitor_watch *w = proxy_monitor_watch(pid, cb, &c);
   CHECK(w == NULL, "watching a dead child unexpectedly succeeded");
   CHECK(!cb_ctx_wait(&c, 100), "callback fired for a failed watch");

   int status = 0;
   waitpid(pid, &status, 0);
   proxy_monitor_unwatch(w); /* no-op on NULL */
}

int
main(void)
{
   CHECK(proxy_monitor_start(), "monitor start failed");

   test_exit_then_destroy();
   test_destroy_before_exit();
   test_exit_before_watch();

   proxy_monitor_shutdown();

   if (failures) {
      fprintf(stderr, "%d check(s) failed\n", failures);
      return 1;
   }
   printf("test_proxy_monitor: all checks passed\n");
   return 0;
}
