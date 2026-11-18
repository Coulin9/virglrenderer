/*
 * Copyright 2026
 * SPDX-License-Identifier: MIT
 */

#include "proxy_worker.h"

#ifdef __APPLE__

#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "proxy_monitor.h"
#include "server/render_spawn.h"

/* Registry of live workers for find/reset; separate from the monitor
 * mutex (which is private to proxy_monitor). */
static once_flag proxy_worker_once = ONCE_FLAG_INIT;
static mtx_t proxy_worker_registry_mtx;
static struct list_head proxy_worker_registry;

static void
proxy_worker_registry_init(void)
{
   mtx_init(&proxy_worker_registry_mtx, mtx_plain);
   list_inithead(&proxy_worker_registry);
}

static void
proxy_worker_monitor_exit(void *udata, bool reaped)
{
   struct proxy_worker *w = udata;
   w->reaped = reaped;
   if (w->on_exit)
      w->on_exit(w, w->udata);
}

static void
proxy_worker_semaphores_destroy(struct proxy_worker *w)
{
   render_wake_t *sems[] = { &w->main_wake, &w->oob_wake, &w->completion_wake };
   for (size_t i = 0; i < ARRAY_SIZE(sems); i++) {
      if (*sems[i] != RENDER_WAKE_NULL) {
         semaphore_destroy(mach_task_self(), *sems[i]);
         *sems[i] = RENDER_WAKE_NULL;
      }
   }
}

static void
proxy_worker_registry_remove(struct proxy_worker *w)
{
   mtx_lock(&proxy_worker_registry_mtx);
   if (w->head.next && w->head.prev)
      list_del(&w->head);
   w->head.next = NULL;
   w->head.prev = NULL;
   mtx_unlock(&proxy_worker_registry_mtx);
}

/* Grace-then-kill reap, matching the server's DESTROY_CONTEXT/RESET
 * semantics: with eof_grace the worker gets 250 ms of orderly teardown
 * (its context socket EOF) before the SIGKILL backstop. */
static void
proxy_worker_reap(struct proxy_worker *w, bool eof_grace)
{
   if (w->reaped)
      return;

   if (eof_grace) {
      struct timespec deadline;
      bool have_deadline = !clock_gettime(CLOCK_MONOTONIC, &deadline);
      if (have_deadline) {
         deadline.tv_nsec += 250000000;
         if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
         }
      }
      while (have_deadline) {
         siginfo_t siginfo = { 0 };
         if (!waitid(P_PID, (id_t)w->pid, &siginfo, WEXITED | WNOHANG) &&
             siginfo.si_pid == w->pid) {
            w->reaped = true;
            return;
         }
         struct timespec now;
         if (clock_gettime(CLOCK_MONOTONIC, &now) ||
             now.tv_sec > deadline.tv_sec ||
             (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
            break;
         const struct timespec delay = { .tv_sec = 0, .tv_nsec = 2000000 };
         nanosleep(&delay, NULL);
      }
   }

   kill(w->pid, SIGKILL);

   siginfo_t siginfo = { 0 };
   while (waitid(P_PID, (id_t)w->pid, &siginfo, WEXITED) && errno == EINTR) {
   }
   w->reaped = true;
}

bool
proxy_worker_spawn(uint32_t ctx_id,
                   uint32_t capset_id,
                   const char *ctx_name,
                   uint32_t init_flags,
                   struct proxy_worker *out)
{
   if (!proxy_monitor_start())
      return false;

   call_once(&proxy_worker_once, proxy_worker_registry_init);

   memset(&out->head, 0, sizeof(out->head));
   out->ctx_id = ctx_id;
   out->pid = -1;
   out->ctx_fd = -1;
   out->main_wake = RENDER_WAKE_NULL;
   out->oob_wake = RENDER_WAKE_NULL;
   out->completion_wake = RENDER_WAKE_NULL;
   out->watch = NULL;
   out->reaped = false;

   int socket_fds[2];
   if (!proxy_socket_pair(socket_fds)) {
      proxy_log("failed to create context socketpair");
      return false;
   }

   mach_port_t ports[RENDER_WAKE_PORT_COUNT] = { MACH_PORT_NULL };
   render_wake_t *sems[RENDER_WAKE_PORT_COUNT] = {
      &out->main_wake,
      &out->oob_wake,
      &out->completion_wake,
   };
   for (int i = 0; i < RENDER_WAKE_PORT_COUNT; i++) {
      if (semaphore_create(mach_task_self(), sems[i], SYNC_POLICY_FIFO, 0))
         goto fail_pre_spawn;
      ports[i] = *sems[i];
   }

   {
      struct render_context_args ctx_args;
      render_init_context_args(&ctx_args, init_flags, capset_id, ctx_id,
                               ctx_name, strnlen(ctx_name, 32), socket_fds[0]);
      out->pid = render_spawn_worker(&ctx_args, ports);
   }
   if (out->pid < 0)
      goto fail_pre_spawn;

   /* the child owns socket_fds[0] now */
   close(socket_fds[0]);
   socket_fds[0] = -1;
   out->ctx_fd = socket_fds[1];

   /* NOTE_EXIT registration fails with ESRCH for a child that already
    * died between spawn and here; treat that as a startup failure and
    * reap it below */
   out->watch =
      proxy_monitor_watch(out->pid, proxy_worker_monitor_exit, out);
   if (!out->watch) {
      close(out->ctx_fd);
      out->ctx_fd = -1;
      kill(out->pid, SIGKILL);
      siginfo_t siginfo = { 0 };
      while (waitid(P_PID, (id_t)out->pid, &siginfo, WEXITED) &&
             errno == EINTR) {
      }
      out->reaped = true;
      goto fail_pre_spawn;
   }

   mtx_lock(&proxy_worker_registry_mtx);
   list_addtail(&out->head, &proxy_worker_registry);
   mtx_unlock(&proxy_worker_registry_mtx);

   return true;

fail_pre_spawn:
   proxy_worker_semaphores_destroy(out);
   if (socket_fds[0] >= 0)
      close(socket_fds[0]);
   if (socket_fds[1] >= 0 && socket_fds[1] != out->ctx_fd)
      close(socket_fds[1]);
   return false;
}

void
proxy_worker_destroy(struct proxy_worker *w)
{
   /* EV_DELETE under the monitor mutex: after this returns no monitor
    * callback can still touch this worker */
   if (w->watch) {
      proxy_monitor_unwatch(w->watch);
      w->watch = NULL;
   }
   proxy_worker_reap(w, true /* eof_grace */);
   proxy_worker_semaphores_destroy(w);
   proxy_worker_registry_remove(w);
}

struct proxy_worker *
proxy_worker_find(uint32_t ctx_id)
{
   struct proxy_worker *found = NULL;
   call_once(&proxy_worker_once, proxy_worker_registry_init);
   mtx_lock(&proxy_worker_registry_mtx);
   list_for_each_entry (struct proxy_worker, w, &proxy_worker_registry,
                        head) {
      if (w->ctx_id == ctx_id) {
         found = w;
         break;
      }
   }
   mtx_unlock(&proxy_worker_registry_mtx);
   return found;
}

void
proxy_worker_kill_all(void)
{
   /* RESET semantics: kill and reap every registered worker; the owning
    * contexts destroy their records (and semaphores) afterwards */
   call_once(&proxy_worker_once, proxy_worker_registry_init);
   mtx_lock(&proxy_worker_registry_mtx);
   list_for_each_entry (struct proxy_worker, w, &proxy_worker_registry,
                        head) {
      if (w->watch) {
         proxy_monitor_unwatch(w->watch);
         w->watch = NULL;
      }
      proxy_worker_reap(w, false /* eof_grace */);
   }
   mtx_unlock(&proxy_worker_registry_mtx);
}

#endif /* __APPLE__ */
