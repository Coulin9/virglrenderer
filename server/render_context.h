/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef RENDER_CONTEXT_H
#define RENDER_CONTEXT_H

#include "render_common.h"

#include <stdatomic.h>

#include "c11/threads.h"

#include "render_ring.h"

enum render_backend_type {
   RENDER_BACKEND_NONE = 0,
   RENDER_BACKEND_VENUS,
   RENDER_BACKEND_NEPTUNE,
};

struct render_context {
   uint32_t ctx_id;
   struct render_socket socket;
   struct list_head head;

   char *name;
   size_t name_len;

   int shmem_fd;
   size_t shmem_size;
   void *shmem_ptr;
   struct render_ring_control *ring; /* == shmem_ptr */
   struct render_ring_buf main_ring;
   struct render_ring_oob oob_ring;
   uint32_t ring_flags; /* RENDER_RING_FLAG_* */
   bool ring_ready;     /* INIT processed; run loop is in ring mode */

   int timeline_count;

   enum render_backend_type backend;

   /* write end of the completion wake channel (eventfd/pipe) handed over
    * by INIT; signalled per the conditional-wake state machine, or always
    * when ring_flags has RENDER_RING_FLAG_ALWAYS_NOTIFY */
   int fence_eventfd;

   /* read ends of the main/oob ring wake channels and the write end of
    * the death pipe (never written; closed on worker exit) */
   int main_wake_read;
   int oob_wake_read;
   int death_write_fd;

   /* out-of-band fence ring consumer: a dedicated thread registers
    * arrival-order-tolerant fences without queueing behind the dispatch
    * loop's in-flight submits */
   thrd_t oob_thread;
   bool oob_thread_created;
   int oob_stop_read;
   int oob_stop_write;

   /* env-gated per-fence latency trace: shared record + seqmap area
    * after the rings; the worker resolves fence_ids through it */
   struct render_ring_lat_area *lat; /* NULL when disabled */
};

struct render_context_args {
   bool valid;

   uint32_t init_flags;

   uint32_t ctx_id;
   char ctx_name[32];

   /* render_context_main always takes ownership even on errors */
   int ctx_fd;
};

bool
render_context_main(const struct render_context_args *args);

void
render_context_update_timeline(struct render_context *ctx,
                               uint32_t ring_idx,
                               uint32_t val);

#endif /* RENDER_CONTEXT_H */
