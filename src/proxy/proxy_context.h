/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef PROXY_CONTEXT_H
#define PROXY_CONTEXT_H

#include "proxy_common.h"
#include "neptune/npt_work_context.h"
#include "server/render_ring.h"
#include "proxy_status_page.h"

#include "c11/threads.h"
#include "virgl_context.h"

/* matches virtio-gpu */
#define PROXY_CONTEXT_TIMELINE_COUNT 64

static_assert(ATOMIC_INT_LOCK_FREE == 2, "proxy renderer requires lock-free atomic_uint");

struct proxy_timeline {
   uint32_t cur_seqno;
   uint32_t next_seqno;
   struct list_head fences;
};

struct proxy_context {
   struct virgl_context base;

   struct proxy_client *client;
   struct proxy_socket socket;

   uint32_t capset_id;
   void *work_context;
   struct npt_work_context_config work_context_config;

   /* Shared submit ring (server/render_ring.h): replaces the per-op
    * socket traffic and the fence socket.  The socket is kept for INIT,
    * SOCKET_REQUEST-covered cold ops and oversized command payloads. */
   mtx_t ring_mutex; /* guards all ring writes and socket request order */
   struct render_ring_control *ring; /* == shmem.ptr */
   struct render_ring_buf main_ring;
   struct render_ring_oob oob_ring;
   bool has_oob_ring;
   _Atomic bool worker_dead;
   int main_wake_write; /* VMM-held write end; worker holds the read end */
   int oob_wake_write;  /* -1 when no oob ring */
   int death_read_fd;   /* readable/HUP once the worker exits */

   /* the worker's ends of the ring pipes, held only until INIT passes
    * them over SCM_RIGHTS (a dup when the platform gives one fd for both
    * ends of a channel) */
   struct {
      int completion_write;
      int main_wake_read;
      int death_write;
      int oob_wake_read;
   } ring_fds;

   bool ring_stats_enabled;

   /* env-gated per-fence latency trace (VIRGL_LATENCY_TRACE=<dir>): the
    * shared region then carries a render_ring_lat_area this context
    * stamps on submit/retire; dumped to <dir> at destroy */
   bool lat_trace_enabled;
   const char *lat_trace_dir;
   struct render_ring_lat_area *lat; /* NULL when disabled */
   uint64_t lat_vmm_wake_ts;         /* sync thread: last poll() return */

   /* this tracks resources early attached in get_blob */
   struct hash_table *resource_table;

   /* the guest status page (NPT_CONTEXT_STATUS_BLOB_ID): owned by the
    * proxy, never forwarded to the worker.  status_resource_table holds
    * the res_ids it was handed out under. */
   _Atomic(struct npt_context_status_page *) status_page;
   int status_fd;
   struct hash_table *status_resource_table;

   /* this is shared with the render worker */
   struct {
      int fd;
      size_t size;
      void *ptr;
   } shmem;

   mtx_t timeline_mutex;
   struct proxy_timeline timelines[PROXY_CONTEXT_TIMELINE_COUNT];
   /* which timelines have fences */
   _Atomic uint64_t timeline_busy_mask;
   /* this points a region of shmem updated by the render worker */
   const volatile atomic_uint *timeline_seqnos;

   mtx_t free_fences_mutex;
   struct list_head free_fences;

   struct {
      /* when VIRGL_RENDERER_THREAD_SYNC is set: the fence signalling
       * channel from create_fence_pipe().  fence_eventfd is the end we
       * poll/drain; fence_eventfd_write is the end the render worker (and
       * the destroy-path stop wake) writes.  On Linux both are the same
       * eventfd; on macOS they are the two ends of a pipe. */
      int fence_eventfd;
      int fence_eventfd_write;

      /* when VIRGL_RENDERER_ASYNC_FENCE_CB is also set */
      thrd_t thread;
      bool created;
      bool stop;
   } sync_thread;
};

#endif /* PROXY_CONTEXT_H */
