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

#include "util/u_thread.h"
#include "virgl_util.h"

#include "render_socket.h"
#include "render_state.h"
#ifdef ENABLE_NEPTUNE
#include "npt_renderer.h"
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
   if (ctx->fence_eventfd < 0)
      return;

   if (ctx->ring_flags & RENDER_RING_FLAG_ALWAYS_NOTIFY) {
      render_ring_fd_signal(ctx->fence_eventfd);
      atomic_fetch_add_explicit(&ctx->ring->stats_worker.v.fence_notifies, 1,
                                memory_order_relaxed);
   } else {
      render_ring_producer_wake(&ctx->ring->vmm_state.value, ctx->fence_eventfd,
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
   uint32_t head;
};

static bool
render_context_oob_has_work(void *opaque)
{
   const struct render_context_oob_consumer *c = opaque;
   return atomic_load_explicit(c->r->tail, memory_order_acquire) != c->head;
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

      const enum render_ring_wait_result res = render_ring_consumer_wait(
         &ctx->ring->oob_state.value, ctx->oob_wake_read, ctx->oob_stop_read,
         render_context_oob_has_work, &consumer, 0 /* spin_ns */,
         -1 /* timeout */, &ctx->ring->stats_worker.v.oob_sleeps);
      if (res == RENDER_RING_WAIT_FD1 || res == RENDER_RING_WAIT_ERROR)
         break;
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
   if (fd_count != (has_oob ? 5 : 4)) {
      render_log("INIT: expected %d fds, got %d", has_oob ? 5 : 4, fd_count);
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
   const int fence_eventfd = fds[1];
   const int main_wake_read = fds[2];
   const int death_write_fd = fds[3];
   const int oob_wake_read = has_oob ? fds[4] : -1;

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

   ctx->shmem_fd = shmem_fd;
   ctx->shmem_size = req->shmem_size;
   ctx->shmem_ptr = shmem_ptr;
   ctx->ring = shmem_ptr;
   ctx->ring_flags = req->ring_flags;
   ctx->main_ring =
      render_ring_main_buf(ctx->ring, shmem_ptr, req->main_ring_offset,
                           req->main_ring_size);
   ctx->timeline_count = RENDER_RING_TIMELINE_COUNT;

   ctx->fence_eventfd = fence_eventfd;
   ctx->main_wake_read = main_wake_read;
   ctx->death_write_fd = death_write_fd;
   ctx->oob_wake_read = oob_wake_read;

   if ((req->ring_flags & RENDER_RING_FLAG_LATENCY_TRACE) &&
       req->lat_ring_size)
      ctx->lat = render_ring_lat_area(shmem_ptr, req->lat_ring_offset);

   if (has_oob) {
      ctx->oob_ring = render_ring_oob_buf(ctx->ring, shmem_ptr,
                                          req->oob_ring_offset,
                                          req->oob_ring_size);
      if (create_fence_pipe(&ctx->oob_stop_read, &ctx->oob_stop_write)) {
         render_log("INIT: failed to create oob stop pipe");
         munmap(shmem_ptr, req->shmem_size);
         ctx->shmem_ptr = NULL;
         ctx->ring = NULL;
         return false;
      }
      if (thrd_create(&ctx->oob_thread, render_context_oob_thread, ctx) !=
          thrd_success) {
         render_log("INIT: oob thread creation failed");
         close(ctx->oob_stop_read);
         close(ctx->oob_stop_write);
         ctx->oob_stop_read = -1;
         ctx->oob_stop_write = -1;
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

      /* ring drained: park on the wake fd, also watching the socket so a
       * VMM close is noticed while we sleep */
      const enum render_ring_wait_result res = render_ring_consumer_wait(
         &ctx->ring->main_state.value, ctx->main_wake_read, ctx->socket.fd,
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
      write_eventfd(ctx->oob_stop_write, 1);
      thrd_join(ctx->oob_thread, NULL);
      ctx->oob_thread_created = false;
   }
   if (ctx->oob_stop_read >= 0)
      close(ctx->oob_stop_read);
   if (ctx->oob_stop_write >= 0)
      close(ctx->oob_stop_write);
   if (ctx->oob_wake_read >= 0)
      close(ctx->oob_wake_read);
   if (ctx->main_wake_read >= 0)
      close(ctx->main_wake_read);
   if (ctx->death_write_fd >= 0)
      close(ctx->death_write_fd);

   /* destroy the context first to join its sync threads and ring threads */
   render_state_destroy_context(ctx->ctx_id);

   if (ctx->shmem_ptr)
      munmap(ctx->shmem_ptr, ctx->shmem_size);
   if (ctx->shmem_fd >= 0)
      close(ctx->shmem_fd);

   if (ctx->fence_eventfd >= 0)
      close(ctx->fence_eventfd);

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

static bool
render_context_init(struct render_context *ctx, const struct render_context_args *args)
{
   memset(ctx, 0, sizeof(*ctx));
   ctx->ctx_id = args->ctx_id;
   render_socket_init(&ctx->socket, args->ctx_fd);
   ctx->shmem_fd = -1;
   ctx->fence_eventfd = -1;
   ctx->main_wake_read = -1;
   ctx->oob_wake_read = -1;
   ctx->death_write_fd = -1;
   ctx->oob_stop_read = -1;
   ctx->oob_stop_write = -1;

   if (!render_context_init_name(ctx, args->ctx_id, args->ctx_name))
      return false;

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
