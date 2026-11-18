/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "render_context.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/event.h>
#endif

#include "util/u_thread.h"
#include "virgl_util.h"

#include "render_socket.h"
#include "render_state.h"
#include "virtgpu_drm.h"
#ifdef ENABLE_NEPTUNE
#include "npt_renderer.h"
#include "neptune-protocol/npt_protocol_transport.h"
#endif

/* Resolve the VMM fence_id for a dequeued fence entry through the
 * latency trace area's (ring_idx, seqno) side map.  The proxy stores
 * seqno then fence_id (release); the acquire on fence_id orders the
 * seqno read after it, and the seqno match rejects stale/aliased
 * entries -- an alias only loses a stamp, never correctness. */
static uint64_t
render_context_lat_fence_id(struct render_context *ctx, uint32_t ring_idx,
                            uint32_t seqno)
{
   const struct render_ring_lat_seqent *se =
      render_ring_lat_seqmap(ctx->lat, ring_idx, seqno);
   const uint64_t fence_id =
      atomic_load_explicit(&se->fence_id, memory_order_acquire);
   if (!fence_id)
      return 0;
   if (atomic_load_explicit(&se->seqno, memory_order_acquire) != seqno)
      return 0;
   return fence_id;
}

/* called when a fence entry is dequeued (main ring or oob ring) */
static void
render_context_lat_recv(struct render_context *ctx, uint32_t ring_idx,
                        uint32_t seqno)
{
   if (!ctx->lat)
      return;

   const uint64_t fence_id =
      render_context_lat_fence_id(ctx, ring_idx, seqno);
   if (!fence_id)
      return;

   const uint32_t idx = (uint32_t)(fence_id & (RENDER_RING_LAT_RECORDS - 1));
   struct render_ring_lat_rec *rec = &ctx->lat->recs[idx];
   if (atomic_load_explicit(&rec->fence_id, memory_order_acquire) != fence_id)
      return;
   atomic_thread_fence(memory_order_seq_cst);
   rec->t_worker_recv = render_ring_now_ns();
}

/* called just before publishing a timeline seqno */
static void
render_context_lat_signal(struct render_context *ctx, uint32_t ring_idx,
                          uint32_t seqno)
{
   if (!ctx->lat)
      return;

   const uint64_t fence_id =
      render_context_lat_fence_id(ctx, ring_idx, seqno);
   if (!fence_id)
      return;

   const uint32_t idx = (uint32_t)(fence_id & (RENDER_RING_LAT_RECORDS - 1));
   struct render_ring_lat_rec *rec = &ctx->lat->recs[idx];
   if (atomic_load_explicit(&rec->fence_id, memory_order_acquire) != fence_id)
      return;
   atomic_thread_fence(memory_order_seq_cst);
   rec->t_worker_signal = render_ring_now_ns();
}

void
render_context_update_timeline(struct render_context *ctx,
                               uint32_t ring_idx,
                               uint32_t seqno)
{
   /* this can be called by the context's main thread and sync threads */
   render_context_lat_signal(ctx, ring_idx, seqno);
   atomic_store_explicit(&ctx->ring->timelines[ring_idx], seqno,
                         memory_order_release);
   if (ctx->completion_wake == RENDER_WAKE_NULL)
      return;

   if (ctx->ring_flags & RENDER_RING_FLAG_ALWAYS_NOTIFY) {
      render_wake_signal(ctx->completion_wake);
      atomic_fetch_add_explicit(&ctx->ring->stats_worker.v.fence_notifies, 1,
                                memory_order_relaxed);
   } else {
      render_ring_producer_wake(&ctx->ring->vmm_state.value,
                                ctx->completion_wake,
                                &ctx->ring->stats_worker.v.fence_notifies);
   }
}

static bool
render_context_dispatch_submit_fence(struct render_context *ctx,
                                     const union render_context_op_request *request,
                                     UNUSED const int *fds,
                                     UNUSED int fd_count)
{
   const struct render_context_op_submit_fence_request *req = &request->submit_fence;

   /* always merge fences */
   assert(!(req->flags & ~VIRGL_RENDERER_FENCE_FLAG_MERGEABLE));
   assert(req->ring_index < (uint32_t)ctx->timeline_count);

   if (!render_state_submit_fence(ctx->ctx_id, VIRGL_RENDERER_FENCE_FLAG_MERGEABLE,
                                  req->ring_index, req->seqno))
      render_log("submit_fence(ring %u) failed", req->ring_index);

   return true;
}

struct render_context_oob_consumer {
   const struct render_ring_oob *r;
   struct render_ring_control *ctl;
   uint32_t head;
};

static bool
render_context_oob_has_work(void *opaque)
{
   const struct render_context_oob_consumer *c = opaque;
   return atomic_load_explicit(c->r->tail, memory_order_acquire) != c->head ||
          render_ring_doorbell_pending(c->ctl);
}

static void
render_context_oob_doorbell_cb(void *opaque, uint32_t ring_id)
{
   struct render_context *ctx = opaque;
   render_state_ring_doorbell(ctx->ctx_id, ring_id);
}

/* Out-of-band fence ring consumer (replaces the fence socket thread):
 * registers arrival-order-tolerant fences the moment they land in the
 * shared slot ring, independent of the dispatch loop's backlog. */
static int
render_context_oob_thread(void *arg)
{
   struct render_context *ctx = arg;

   u_thread_setname("virgl-fence");

   struct render_context_oob_consumer consumer = {
      .r = &ctx->oob_ring,
      .ctl = ctx->ring,
      .head = 0,
   };

   for (;;) {
      struct render_ring_oob_slot slots[64];
      const uint32_t n =
         render_ring_oob_read(&ctx->oob_ring, &consumer.head, slots,
                              ARRAY_SIZE(slots));
      if (n == UINT32_MAX) {
         render_log("oob fence ring: head/tail corruption; thread exiting");
         break;
      }

      bool bad = false;
      for (uint32_t i = 0; i < n; i++) {
         const struct render_ring_fence *f = &slots[i].fence;
         if (f->ring_idx >= (uint32_t)ctx->timeline_count) {
            render_log("oob fence ring: bad ring_idx %u", f->ring_idx);
            bad = true;
            break;
         }
         render_context_lat_recv(ctx, f->ring_idx, f->seqno);
         if (!render_state_submit_fence(ctx->ctx_id,
                                        VIRGL_RENDERER_FENCE_FLAG_MERGEABLE,
                                        f->ring_idx, f->seqno))
            render_log("oob fence ring: submit_fence(ring %u) failed",
                       f->ring_idx);
      }
      render_ring_oob_commit(&ctx->oob_ring, consumer.head);
      if (bad)
         break;

      /* MMIO doorbells share this consumer's wake domain: drain the
       * mask after the fence slots so a bit can't be picked up while
       * oob_state still reads ACTIVE between iterations. */
      render_ring_doorbell_drain(ctx->ring,
                                 render_context_oob_doorbell_cb, ctx);

#if defined(__APPLE__)
      const enum render_ring_wait_result res = render_ring_consumer_wait(
         &ctx->ring->oob_state.value, ctx->oob_wake,
         render_context_oob_has_work, &consumer, 0 /* spin_ns */,
         -1 /* timeout */, &ctx->ring->stats_worker.v.oob_sleeps);
      if (res == RENDER_RING_WAIT_ERROR)
         break;
      /* WORK or WOKEN: a stop request arrives via the watcher as a
       * wake signal */
      if (atomic_load_explicit(&ctx->stopping, memory_order_acquire))
         break;
#else
      const enum render_ring_wait_result res = render_ring_consumer_wait_fd(
         &ctx->ring->oob_state.value, ctx->oob_wake, ctx->oob_stop_read,
         render_context_oob_has_work, &consumer, 0 /* spin_ns */,
         -1 /* timeout */, &ctx->ring->stats_worker.v.oob_sleeps);
      if (res == RENDER_RING_WAIT_FD1 || res == RENDER_RING_WAIT_ERROR)
         break;
#endif
   }

   return 0;
}

static bool
render_context_dispatch_submit_cmd(struct render_context *ctx,
                                   const union render_context_op_request *request,
                                   UNUSED const int *fds,
                                   UNUSED int fd_count)
{
   const struct render_context_op_submit_cmd_request *req = &request->submit_cmd;
   void *cmd = (void *)req->cmd;
   if (req->size > sizeof(req->cmd)) {
      cmd = malloc(req->size);
      if (!cmd)
         return true;

      const size_t inlined = sizeof(req->cmd);
      const size_t remain = req->size - inlined;

      memcpy(cmd, req->cmd, inlined);
      if (!render_socket_receive_data(&ctx->socket, (char *)cmd + inlined, remain)) {
         ctx->socket.peer_closed = false;
         free(cmd);
         return false;
      }
   }

   bool ok = render_state_submit_cmd(ctx->ctx_id, cmd, req->size);

   if (cmd != req->cmd)
      free(cmd);

   return ok;
}

static bool
render_context_dispatch_destroy_resource(struct render_context *ctx,
                                         const union render_context_op_request *req,
                                         UNUSED const int *fds,
                                         UNUSED int fd_count)
{
   render_state_destroy_resource(ctx->ctx_id, req->destroy_resource.res_id);
   return true;
}

static bool
render_context_dispatch_import_resource(struct render_context *ctx,
                                        const union render_context_op_request *request,
                                        const int *fds,
                                        int fd_count)
{
   if (fd_count != 1) {
      render_log("failed to attach resource with fd_count %d", fd_count);
      return false;
   }

   /* classic 3d resource with valid size reuses the blob import path here */
   const struct render_context_op_import_resource_request *req =
      &request->import_resource;
   return render_state_import_resource(ctx->ctx_id, req->res_id, req->fd_type, fds[0],
                                       req->size, req->export_format);
}

static bool
render_context_dispatch_create_resource(struct render_context *ctx,
                                        const union render_context_op_request *request,
                                        UNUSED const int *fds,
                                        UNUSED int fd_count)
{
   const struct render_context_op_create_resource_request *req =
      &request->create_resource;
   struct render_context_op_create_resource_reply reply = {
      .fd_type = VIRGL_RESOURCE_FD_INVALID,
   };
   int res_fd;
   bool ok = render_state_create_resource(ctx->ctx_id, req->res_id, req->blob_id,
                                          req->blob_size, req->blob_flags, &reply.fd_type,
                                          &res_fd, &reply.map_info, &reply.vulkan_info,
                                          &reply.export_format);
   if (!ok)
      return render_socket_send_reply(&ctx->socket, &reply, sizeof(reply));

   ok =
      render_socket_send_reply_with_fds(&ctx->socket, &reply, sizeof(reply), &res_fd, 1);
   close(res_fd);

   return ok;
}

/* Bridge from the Neptune context to the completion wake without
 * exposing render internals: npt_completion code calls this when a
 * guest waiter needs an interrupt injected. */
static void
render_context_status_wake(void *opaque)
{
   struct render_context *ctx = opaque;
   if (ctx->completion_wake != RENDER_WAKE_NULL)
      render_wake_signal(ctx->completion_wake);
}

static bool
render_context_dispatch_init(struct render_context *ctx,
                             const union render_context_op_request *request,
                             const int *fds,
                             int fd_count)
{
   if (ctx->ring_ready) {
      render_log("duplicate INIT");
      return false;
   }

   const struct render_context_op_init_request *req = &request->init;
   const bool has_oob = req->oob_ring_size != 0;
   const bool has_status =
      (req->flags & VIRGL_RENDERER_CONTEXT_FLAG_CAPSET_ID_MASK) ==
      VIRTGPU_DRM_CAPSET_NEPTUNE;
#if defined(__APPLE__)
   /* the wake semaphores arrive as registered ports at spawn; INIT
    * carries only the ring shmem fd plus the status page on Neptune */
   const int expected_fds = has_status ? 2 : 1;
#else
   const int expected_fds = (has_oob ? 5 : 4) + (has_status ? 1 : 0);
#endif
   if (fd_count != expected_fds) {
      render_log("INIT: expected %d fds, got %d", expected_fds, fd_count);
      return false;
   }

   /* validate the advertised layout before touching the region */
   if (!render_ring_layout_valid(req->shmem_size, req->main_ring_offset,
                                 req->main_ring_size, req->oob_ring_offset,
                                 req->oob_ring_size, req->lat_ring_offset,
                                 req->lat_ring_size)) {
      render_log("INIT: invalid ring layout (shmem %zu main %u+%u oob %u+%u)",
                 req->shmem_size, req->main_ring_offset, req->main_ring_size,
                 req->oob_ring_offset, req->oob_ring_size);
      return false;
   }

   /* MSG_CMSG_CLOEXEC is a no-op on macOS: mark the received fds so a
    * spawned child can never inherit the death pipe (or anything else)
    * and mask a worker exit */
   for (int i = 0; i < fd_count; i++) {
      const int fdflags = fcntl(fds[i], F_GETFD);
      if (fdflags >= 0)
         fcntl(fds[i], F_SETFD, fdflags | FD_CLOEXEC);
   }

   const int shmem_fd = fds[0];
#if !defined(__APPLE__)
   const int fence_eventfd = fds[1];
   const int main_wake_read = fds[2];
   const int death_write_fd = fds[3];
   const int oob_wake_read = has_oob ? fds[4] : -1;
#endif
   /* the status page fd always travels last when present */
   const int status_fd = has_status ? fds[expected_fds - 1] : -1;

   void *shmem_ptr =
      mmap(NULL, req->shmem_size, PROT_READ | PROT_WRITE, MAP_SHARED, shmem_fd, 0);
   if (shmem_ptr == MAP_FAILED)
      return false;

   if (!render_state_create_context(ctx, req->flags, ctx->name_len, ctx->name)) {
      munmap(shmem_ptr, req->shmem_size);
      return false;
   }

#ifdef ENABLE_NEPTUNE
   if (ctx->backend == RENDER_BACKEND_NEPTUNE)
      npt_renderer_set_work_context(ctx->ctx_id, &req->work_context);
#endif

#ifdef ENABLE_NEPTUNE
   if (status_fd >= 0) {
      void *status_ptr = mmap(NULL, NPT_CONTEXT_STATUS_PAGE_SIZE,
                              PROT_READ | PROT_WRITE, MAP_SHARED, status_fd,
                              0);
      if (status_ptr == MAP_FAILED ||
          ((struct npt_context_status_page *)status_ptr)->magic !=
             NPT_CONTEXT_STATUS_MAGIC) {
         render_log("INIT: status page mapping failed");
         if (status_ptr != MAP_FAILED)
            munmap(status_ptr, NPT_CONTEXT_STATUS_PAGE_SIZE);
         close(status_fd);
         munmap(shmem_ptr, req->shmem_size);
         return false;
      }
      ctx->status_fd = status_fd;
      ctx->status_ptr = status_ptr;
      if (ctx->backend == RENDER_BACKEND_NEPTUNE)
         npt_renderer_set_status_page(ctx->ctx_id, ctx->status_ptr,
                                      render_context_status_wake, ctx);
   }
#endif

   ctx->shmem_fd = shmem_fd;
   ctx->shmem_size = req->shmem_size;
   ctx->shmem_ptr = shmem_ptr;
   ctx->ring = shmem_ptr;
   ctx->ring_flags = req->ring_flags;
   ctx->main_ring =
      render_ring_main_buf(ctx->ring, shmem_ptr, req->main_ring_offset,
                           req->main_ring_size);
   ctx->timeline_count = RENDER_RING_TIMELINE_COUNT;

#if !defined(__APPLE__)
   ctx->completion_wake = fence_eventfd;
   ctx->main_wake = main_wake_read;
   ctx->death_write_fd = death_write_fd;
   ctx->oob_wake = oob_wake_read;
#endif

   if ((req->ring_flags & RENDER_RING_FLAG_LATENCY_TRACE) &&
       req->lat_ring_size)
      ctx->lat = render_ring_lat_area(shmem_ptr, req->lat_ring_offset);

   if (has_oob) {
      ctx->oob_ring = render_ring_oob_buf(ctx->ring, shmem_ptr,
                                          req->oob_ring_offset,
                                          req->oob_ring_size);
#if !defined(__APPLE__)
      if (create_fence_pipe(&ctx->oob_stop_read, &ctx->oob_stop_write)) {
         render_log("INIT: failed to create oob stop pipe");
         munmap(shmem_ptr, req->shmem_size);
         ctx->shmem_ptr = NULL;
         ctx->ring = NULL;
         return false;
      }
#endif
      if (thrd_create(&ctx->oob_thread, render_context_oob_thread, ctx) !=
          thrd_success) {
         render_log("INIT: oob thread creation failed");
#if !defined(__APPLE__)
         close(ctx->oob_stop_read);
         close(ctx->oob_stop_write);
         ctx->oob_stop_read = -1;
         ctx->oob_stop_write = -1;
#endif
         munmap(shmem_ptr, req->shmem_size);
         ctx->shmem_ptr = NULL;
         ctx->ring = NULL;
         return false;
      }
      ctx->oob_thread_created = true;
   }

   ctx->ring_ready = true;

   return true;
}

static bool
render_context_dispatch_nop(UNUSED struct render_context *ctx,
                            UNUSED const union render_context_op_request *req,
                            UNUSED const int *fds,
                            UNUSED int fd_count)
{
   return true;
}

struct render_context_dispatch_entry {
   size_t expect_size;
   int max_fd_count;
   bool (*dispatch)(struct render_context *ctx,
                    const union render_context_op_request *req,
                    const int *fds,
                    int fd_count);
};

static const struct render_context_dispatch_entry
   render_context_dispatch_table[RENDER_CONTEXT_OP_COUNT] = {
#define RENDER_CONTEXT_DISPATCH(NAME, name, max_fd)                                      \
   [RENDER_CONTEXT_OP_##                                                                 \
      NAME] = { .expect_size = sizeof(struct render_context_op_##name##_request),        \
                .max_fd_count = (max_fd),                                                \
                .dispatch = render_context_dispatch_##name }
      RENDER_CONTEXT_DISPATCH(NOP, nop, 0),
      RENDER_CONTEXT_DISPATCH(INIT, init, 8),
      RENDER_CONTEXT_DISPATCH(CREATE_RESOURCE, create_resource, 0),
      RENDER_CONTEXT_DISPATCH(IMPORT_RESOURCE, import_resource, 1),
      RENDER_CONTEXT_DISPATCH(DESTROY_RESOURCE, destroy_resource, 0),
      RENDER_CONTEXT_DISPATCH(SUBMIT_CMD, submit_cmd, 0),
      RENDER_CONTEXT_DISPATCH(SUBMIT_FENCE, submit_fence, 0),
#undef RENDER_CONTEXT_DISPATCH
   };

static bool
render_context_dispatch(struct render_context *ctx)
{
   ctx->socket.peer_closed = false;

   union render_context_op_request req;
   size_t req_size;
   int req_fds[8];
   int req_fd_count;
   if (!render_socket_receive_request_with_fds(&ctx->socket, &req, sizeof(req), &req_size,
                                               req_fds, ARRAY_SIZE(req_fds),
                                               &req_fd_count))
      return false;

   assert((unsigned int)req_fd_count <= ARRAY_SIZE(req_fds));

   if (req.header.op >= RENDER_CONTEXT_OP_COUNT) {
      render_log("invalid context op %d", req.header.op);
      goto fail;
   }

   const struct render_context_dispatch_entry *entry =
      &render_context_dispatch_table[req.header.op];
   if (entry->expect_size != req_size || entry->max_fd_count < req_fd_count) {
      render_log("invalid request size (%zu) or fd count (%d) for context op %d",
                 req_size, req_fd_count, req.header.op);
      goto fail;
   }

   const bool ok = entry->dispatch(ctx, &req, req_fds, req_fd_count);
   if (!ok) {
      render_log("failed to dispatch context op %d", req.header.op);
      goto fail;
   }

   return true;

fail:
   for (int i = 0; i < req_fd_count; i++)
      close(req_fds[i]);
   return false;
}

struct render_context_main_consumer {
   const struct render_ring_buf *r;
   uint32_t head;
};

static bool
render_context_main_has_work(void *opaque)
{
   const struct render_context_main_consumer *c = opaque;
   return atomic_load_explicit(c->r->tail, memory_order_acquire) != c->head;
}

static bool
render_context_ring_run(struct render_context *ctx)
{
   struct render_context_main_consumer consumer = {
      .r = &ctx->main_ring,
      .head = 0,
   };

   uint64_t spin_ns = RENDER_RING_DEFAULT_SPIN_NS;
   const char *spin_env = getenv("VIRGL_PROXY_RING_SPIN_NS");
   if (spin_env && *spin_env)
      spin_ns = strtoull(spin_env, NULL, 0);

   for (;;) {
      /* drain the main ring; the VMM publishes entries before any
       * socket bytes, so the ring always comes first */
      for (;;) {
         uint32_t op, size;
         const uint8_t *payload;
         const enum render_ring_read_result res = render_ring_read(
            &ctx->main_ring, &consumer.head, &op, &payload, &size);
         if (res == RENDER_RING_READ_CORRUPT) {
            render_log("main ring: corrupt entry at head %u; terminating",
                       consumer.head);
            return false;
         }
         if (res == RENDER_RING_READ_EMPTY)
            break;

         switch (op) {
         case RENDER_RING_OP_SUBMIT_CMD:
            /* a failed submit kills the context, matching the old
             * socket dispatch behavior */
            if (!render_state_submit_cmd(ctx->ctx_id, (void *)payload, size)) {
               render_log("main ring: submit_cmd failed");
               return false;
            }
            break;
         case RENDER_RING_OP_SUBMIT_CMD_SOCKET: {
            uint32_t wire_size;
            memcpy(&wire_size, payload, sizeof(wire_size));
            if (wire_size > RENDER_RING_MAX_SOCKET_CMD) {
               render_log("main ring: socket cmd size %u out of bounds",
                          wire_size);
               return false;
            }
            void *cmd = malloc(wire_size);
            if (!cmd)
               return false;
            if (!render_socket_receive_data(&ctx->socket, cmd, wire_size)) {
               free(cmd);
               return ctx->socket.peer_closed;
            }
            const bool ok = render_state_submit_cmd(ctx->ctx_id, cmd, wire_size);
            free(cmd);
            if (!ok) {
               render_log("main ring: socket submit_cmd failed");
               return false;
            }
            break;
         }
         case RENDER_RING_OP_SUBMIT_FENCE: {
            struct render_ring_fence f;
            memcpy(&f, payload, sizeof(f));
            if (f.ring_idx >= (uint32_t)ctx->timeline_count) {
               render_log("main ring: bad fence ring_idx %u", f.ring_idx);
               return false;
            }
            render_context_lat_recv(ctx, f.ring_idx, f.seqno);
            if (!render_state_submit_fence(ctx->ctx_id,
                                           VIRGL_RENDERER_FENCE_FLAG_MERGEABLE,
                                           f.ring_idx, f.seqno))
               render_log("main ring: submit_fence(ring %u) failed",
                          f.ring_idx);
            break;
         }
         case RENDER_RING_OP_SOCKET_REQUEST:
            /* publish head first so the producer regains the space; the
             * marker covers exactly one socket request, keeping cold ops
             * ordered relative to ring submits */
            render_ring_commit(&ctx->main_ring, consumer.head);
            if (!render_context_dispatch(ctx))
               return ctx->socket.peer_closed;
            continue;
         default:
            return false;
         }

         render_ring_commit(&ctx->main_ring, consumer.head);
      }
      render_ring_commit(&ctx->main_ring, consumer.head);

#if defined(__APPLE__)
      /* ring drained: park on the wake semaphore.  The watcher thread
       * turns VMM exit / socket EOF / stray socket data into stopping
       * plus a semaphore signal. */
      const enum render_ring_wait_result res = render_ring_consumer_wait(
         &ctx->ring->main_state.value, ctx->main_wake,
         render_context_main_has_work, &consumer, spin_ns, -1 /* timeout */,
         &ctx->ring->stats_worker.v.main_sleeps);
      if (res == RENDER_RING_WAIT_ERROR)
         return false;

      const int stopping =
         atomic_load_explicit(&ctx->stopping, memory_order_acquire);
      if (stopping)
         return stopping == 1; /* 1 = clean close, 2 = protocol error */
#else
      /* ring drained: park on the wake fd, also watching the socket so a
       * VMM close is noticed while we sleep */
      const enum render_ring_wait_result res = render_ring_consumer_wait_fd(
         &ctx->ring->main_state.value, ctx->main_wake, ctx->socket.fd,
         render_context_main_has_work, &consumer, spin_ns, -1 /* timeout */,
         &ctx->ring->stats_worker.v.main_sleeps);
      if (res == RENDER_RING_WAIT_ERROR)
         return false;

      if (res == RENDER_RING_WAIT_FD1) {
         /* socket readable while the ring is empty: the VMM always
          * publishes its marker before socket bytes, so stray data is a
          * protocol error and EOF is a clean close */
         char b;
         const ssize_t r = recv(ctx->socket.fd, &b, 1, MSG_PEEK);
         if (r == 0)
            return true; /* peer closed */
         if (r > 0) {
            render_log("main ring: unexpected socket data; terminating");
            return false;
         }
         if (errno != EINTR && errno != EAGAIN) {
            render_log("main ring: socket peek failed: %s", strerror(errno));
            return false;
         }
      }
#endif
   }

   return true;
}

static bool
render_context_run(struct render_context *ctx)
{
   /* INIT arrives as a plain socket request; everything after it goes
    * through the shared ring */
   while (!ctx->ring_ready) {
      if (!render_context_dispatch(ctx))
         return ctx->socket.peer_closed;
   }

   return render_context_ring_run(ctx);
}

static void
render_context_fini(struct render_context *ctx)
{
   /* stop the oob consumer before the backend context goes away: its
    * thread calls into render_state and must be joined first */
   if (ctx->oob_thread_created) {
#if defined(__APPLE__)
      atomic_store_explicit(&ctx->stopping, 1, memory_order_release);
      render_wake_signal(ctx->oob_wake);
#else
      write_eventfd(ctx->oob_stop_write, 1);
#endif
      thrd_join(ctx->oob_thread, NULL);
      ctx->oob_thread_created = false;
   }
#if !defined(__APPLE__)
   if (ctx->oob_stop_read >= 0)
      close(ctx->oob_stop_read);
   if (ctx->oob_stop_write >= 0)
      close(ctx->oob_stop_write);
   if (ctx->oob_wake != RENDER_WAKE_NULL)
      close(ctx->oob_wake);
   if (ctx->main_wake != RENDER_WAKE_NULL)
      close(ctx->main_wake);
   if (ctx->death_write_fd >= 0)
      close(ctx->death_write_fd);
#endif

   /* destroy the context first to join its sync threads and ring threads */
   render_state_destroy_context(ctx->ctx_id);

#ifdef ENABLE_NEPTUNE
   if (ctx->status_ptr)
      munmap(ctx->status_ptr, NPT_CONTEXT_STATUS_PAGE_SIZE);
#endif
   if (ctx->status_fd >= 0)
      close(ctx->status_fd);
   if (ctx->shmem_ptr)
      munmap(ctx->shmem_ptr, ctx->shmem_size);
   if (ctx->shmem_fd >= 0)
      close(ctx->shmem_fd);

#if !defined(__APPLE__)
   if (ctx->completion_wake != RENDER_WAKE_NULL)
      close(ctx->completion_wake);
#else
   if (ctx->watcher_created) {
      /* ask the watcher to leave its kevent loop and join it */
      struct kevent kev;
      EV_SET(&kev, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
      kevent(ctx->watcher_kq, &kev, 1, NULL, 0, NULL);
      thrd_join(ctx->watcher_thread, NULL);
      ctx->watcher_created = false;
   }
   if (ctx->watcher_kq >= 0)
      close(ctx->watcher_kq);
   /* our send rights on the wake semaphores die with the task; drop
    * them now so a long-lived test process does not leak them */
   if (ctx->main_wake != RENDER_WAKE_NULL)
      mach_port_deallocate(mach_task_self(), ctx->main_wake);
   if (ctx->oob_wake != RENDER_WAKE_NULL)
      mach_port_deallocate(mach_task_self(), ctx->oob_wake);
   if (ctx->completion_wake != RENDER_WAKE_NULL)
      mach_port_deallocate(mach_task_self(), ctx->completion_wake);
#endif

   if (ctx->name)
      free(ctx->name);

   render_socket_fini(&ctx->socket);
}

static void
render_context_set_thread_name(uint32_t ctx_id, UNUSED const char *ctx_name)
{
   char thread_name[16];

#pragma GCC diagnostic push
#if !defined(__clang__) || __clang_major__ >= 18
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
   /* context name may match guest process name, so use a generic name in
    * production/release builds.
    */
#ifndef NDEBUG
   snprintf(thread_name, ARRAY_SIZE(thread_name), "virgl-%d-%s", ctx_id, ctx_name);
#else
   snprintf(thread_name, ARRAY_SIZE(thread_name), "virgl-%d-gpu_renderer", ctx_id);
#endif
#pragma GCC diagnostic pop

   u_thread_setname(thread_name);
}

static const char *ctx_name_expansions[] = {
   "DOOMEternalx64vk.exe",
};

static bool
render_context_init_name(struct render_context *ctx,
                         uint32_t ctx_id,
                         const char *ctx_name)
{
   /* Linux guests may only pass the first 15 chars of a guest application name,
    * plus a null terminator. In that case, attempt substring matching and
    * expansion to enable proper dri-conf lookup in native mesa drivers.
    */
   static const size_t truncated_name_len = 15;
   size_t name_len = strlen(ctx_name);
   if (name_len == truncated_name_len) {
      for (uint32_t i = 0; i < ARRAY_SIZE(ctx_name_expansions); i++) {
         const char *full = ctx_name_expansions[i];
         if (!strncmp(ctx_name, full, truncated_name_len)) {
            ctx_name = full;
            name_len = strlen(full);
            break;
         }
      }
   }

   ctx->name_len = name_len;
   ctx->name = malloc(name_len + 1);
   if (!ctx->name)
      return false;

   strcpy(ctx->name, ctx_name);

   /* Overrides the executable name used only by mesa to load app-specific
    * driver configuration.
    */
   setenv("MESA_DRICONF_EXECUTABLE_OVERRIDE", ctx->name, 0);

   /* Host Mesa still sees the process name as "virgl_render_server" unless
    * additionally overridden by setenv("MESA_PROCESS_NAME", ...).
    */
   render_context_set_thread_name(ctx_id, ctx->name);

   return true;
}

#if defined(__APPLE__)

/* Watch the VMM through a kqueue so a dead or closing peer can never
 * strand the parked ring consumers: EVFILT_PROC NOTE_EXIT on the parent
 * and EVFILT_READ EV_EOF on the context socket both become a stop
 * request plus a semaphore signal.  Stray socket data while the main
 * consumer is parked on an empty ring is a protocol violation: the VMM
 * always publishes a SOCKET_REQUEST marker before socket bytes, and a
 * published marker makes the ring non-empty while a consumed marker is
 * being serviced with the consumer state still ACTIVE/WAKING -- so
 * SLEEPING + empty ring + readable data can only be stray. */
static int
render_context_watcher_thread(void *arg)
{
   struct render_context *ctx = arg;
   u_thread_setname("virgl-watch");

   int reason = 0;
   for (;;) {
      struct kevent ev;
      const int n = kevent(ctx->watcher_kq, NULL, 0, &ev, 1, NULL);
      if (n < 0) {
         if (errno == EINTR)
            continue;
         break;
      }
      if (n == 0)
         continue;
      if (ev.filter == EVFILT_USER)
         break; /* fini */
      if (ev.filter == EVFILT_PROC) {
         if (ev.fflags & NOTE_EXIT)
            reason = 1;
         break;
      }
      if (ev.filter == EVFILT_READ) {
         if (ev.flags & EV_EOF) {
            reason = 1;
            break;
         }
         /* ev.data is the pending byte count: stray socket data while
          * the main consumer is parked on an empty ring is a protocol
          * violation (a SOCKET_REQUEST marker would keep the ring
          * non-empty, and a consumed marker being serviced leaves the
          * consumer state ACTIVE/WAKING) */
         if (ev.data > 0 && ctx->ring &&
             atomic_load_explicit(&ctx->ring->main_state.value,
                                  memory_order_acquire) == RENDER_RING_SLEEPING &&
             !render_ring_has_entries(
                &ctx->main_ring,
                atomic_load_explicit(ctx->main_ring.head,
                                     memory_order_acquire))) {
            render_log("context socket: unexpected data; terminating");
            reason = 2;
            break;
         }
      }
   }

   if (reason) {
      atomic_store_explicit(&ctx->stopping, reason, memory_order_release);
      render_wake_signal(ctx->main_wake);
      render_wake_signal(ctx->oob_wake);
   }

   return 0;
}

static bool
render_context_watcher_start(struct render_context *ctx)
{
   ctx->watcher_kq = kqueue();
   if (ctx->watcher_kq < 0) {
      render_log("failed to create peer watcher kqueue");
      return false;
   }

   struct kevent kev[3];
   EV_SET(&kev[0], getppid(), EVFILT_PROC, EV_ADD | EV_ENABLE, NOTE_EXIT, 0,
          NULL);
   EV_SET(&kev[1], ctx->socket.fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0,
          NULL);
   EV_SET(&kev[2], 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
   if (kevent(ctx->watcher_kq, kev, ARRAY_SIZE(kev), NULL, 0, NULL)) {
      /* ESRCH: the VMM already exited */
      if (errno == ESRCH) {
         render_log("VMM already gone at watcher start; exiting");
         close(ctx->watcher_kq);
         ctx->watcher_kq = -1;
         return false;
      }
      render_log("failed to register peer watcher: %s", strerror(errno));
      close(ctx->watcher_kq);
      ctx->watcher_kq = -1;
      return false;
   }

   if (thrd_create(&ctx->watcher_thread, render_context_watcher_thread,
                   ctx) != thrd_success) {
      render_log("failed to create peer watcher thread");
      close(ctx->watcher_kq);
      ctx->watcher_kq = -1;
      return false;
   }
   ctx->watcher_created = true;
   return true;
}

/* Retrieve the wake semaphores the VMM handed over at spawn time as this
 * task's registered ports (RENDER_WAKE_PORT_* order). */
static bool
render_context_lookup_wake_ports(struct render_context *ctx)
{
   mach_port_array_t ports = NULL;
   mach_msg_type_number_t count = 0;
   const kern_return_t kr =
      mach_ports_lookup(mach_task_self(), &ports, &count);
   if (kr != KERN_SUCCESS || count < RENDER_WAKE_PORT_COUNT ||
       !ports[RENDER_WAKE_PORT_MAIN] || !ports[RENDER_WAKE_PORT_OOB] ||
       !ports[RENDER_WAKE_PORT_COMPLETION]) {
      render_log("missing registered wake ports (kr %d, count %u)", (int)kr,
                 (unsigned)count);
      return false;
   }

   ctx->main_wake = ports[RENDER_WAKE_PORT_MAIN];
   ctx->oob_wake = ports[RENDER_WAKE_PORT_OOB];
   ctx->completion_wake = ports[RENDER_WAKE_PORT_COMPLETION];
   vm_deallocate(mach_task_self(), (vm_address_t)ports,
                 count * sizeof(mach_port_t));
   return true;
}

#endif /* __APPLE__ */

static bool
render_context_init(struct render_context *ctx, const struct render_context_args *args)
{
   memset(ctx, 0, sizeof(*ctx));
   ctx->ctx_id = args->ctx_id;
   render_socket_init(&ctx->socket, args->ctx_fd);
   ctx->shmem_fd = -1;
   ctx->status_fd = -1;
   ctx->completion_wake = RENDER_WAKE_NULL;
   ctx->main_wake = RENDER_WAKE_NULL;
   ctx->oob_wake = RENDER_WAKE_NULL;
#if defined(__APPLE__)
   ctx->watcher_kq = -1;
#else
   ctx->death_write_fd = -1;
   ctx->oob_stop_read = -1;
   ctx->oob_stop_write = -1;
#endif

   if (!render_context_init_name(ctx, args->ctx_id, args->ctx_name))
      return false;

#if defined(__APPLE__)
   if (!render_context_lookup_wake_ports(ctx))
      return false;
   if (!render_context_watcher_start(ctx))
      return false;
#endif

   return true;
}

bool
render_context_main(const struct render_context_args *args)
{
   struct render_context ctx;

   assert(args->valid && args->ctx_id && args->ctx_fd >= 0);

   if (!render_state_init(args->init_flags)) {
      close(args->ctx_fd);
      return false;
   }

   if (!render_context_init(&ctx, args)) {
      render_state_fini();
      close(args->ctx_fd);
      return false;
   }

   const bool ok = render_context_run(&ctx);
   render_context_fini(&ctx);

   render_state_fini();

   return ok;
}
