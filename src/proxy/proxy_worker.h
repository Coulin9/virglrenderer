/*
 * Copyright 2026
 * SPDX-License-Identifier: MIT
 */

#ifndef PROXY_WORKER_H
#define PROXY_WORKER_H

#include "proxy_common.h"

#include "server/render_ring.h"

#include "c11/threads.h"

struct proxy_monitor_watch;

/* A context worker spawned directly by the proxy (macOS only).  The
 * worker binary is still RENDER_SERVER_EXEC_PATH in --worker-context-*
 * mode; the ring wake primitives are Mach semaphores handed over at
 * spawn as the child's task registered ports. */
struct proxy_worker {
   uint32_t ctx_id;
   pid_t pid;
   int ctx_fd; /* proxy end of the context socketpair (peer-owned) */
   render_wake_t main_wake, oob_wake, completion_wake;

   /* called from the monitor thread when the child exits; set before
    * proxy_worker_spawn so no exit can slip past unobserved */
   void (*on_exit)(struct proxy_worker *w, void *udata);
   void *udata;

   struct list_head head;
   struct proxy_monitor_watch *watch; /* owned by proxy_monitor.c */
   bool reaped;
};

bool
proxy_worker_spawn(uint32_t ctx_id,
                   uint32_t capset_id,
                   const char *ctx_name,
                   uint32_t init_flags,
                   struct proxy_worker *out);

void
proxy_worker_destroy(struct proxy_worker *w);

struct proxy_worker *
proxy_worker_find(uint32_t ctx_id);

void
proxy_worker_kill_all(void);

#endif /* PROXY_WORKER_H */
