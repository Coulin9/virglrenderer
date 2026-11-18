/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "proxy_context.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <sched.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "virtgpu_drm.h"

#include "server/render_protocol.h"
#include "util/anon_file.h"
#include "util/bitscan.h"
#include "virgl_fence.h"

#include "proxy_client.h"
#include "proxy_doorbell.h"
#include "neptune/npt_work_context.h"

/* Wake primitives and the worker-exit path differ per platform: macOS
 * spawns the worker directly and passes Mach semaphores as registered
 * ports; other platforms hand pipe/eventfd pairs over INIT. */
static inline render_wake_t
proxy_context_main_wake(const struct proxy_context *ctx)
{
#if defined(__APPLE__)
   return ctx->worker.main_wake;
#else
   return ctx->main_wake_write;
#endif
}

static inline render_wake_t
proxy_context_oob_wake(const struct proxy_context *ctx)
{
#if defined(__APPLE__)
   return ctx->worker.oob_wake;
#else
   return ctx->oob_wake_write;
#endif
}

/* Nudge the sync thread out of its wait so it can run a pass. */
static inline void
proxy_context_nudge_sync(struct proxy_context *ctx)
{
#if defined(__APPLE__)
   render_wake_signal(ctx->worker.completion_wake);
#else
   write_eventfd(ctx->sync_thread.fence_eventfd_write, 1);
#endif
}

#if defined(__APPLE__)
/* Monitor thread callback: the monitor already reaped and logged the
 * child.  Death ordering: mark the guest-visible status page lost
 * first, then seq_cst-store worker_dead (pairs with the submit_fence
 * post-publish seq_cst load), then wake the sync thread for the
 * force-retire pass. */
static void
proxy_context_on_worker_exit(struct proxy_worker *worker, void *udata)
{
   struct proxy_context *ctx = udata;

   (void)worker;
   struct npt_context_status_page *page =
      atomic_load_explicit(&ctx->status_page, memory_order_seq_cst);
   if (page)
      proxy_status_page_mark_lost(page, NPT_CONTEXT_STATUS_LOST_WORKER_EXIT,
                                  render_ring_now_ns());
   atomic_store_explicit(&ctx->worker_dead, true, memory_order_seq_cst);
   /* Wake guest COUNTER_WAIT sleepers directly: the dead worker can no
    * longer set irq_pending, and only a config interrupt or a
    * COUNTER_WAIT slice timeout re-checks the lost word. */
   if (page && proxy_renderer.cbs->context_irq)
      proxy_renderer.cbs->context_irq(ctx->base.ctx_id);
   render_wake_signal(ctx->worker.completion_wake);
}
#endif

struct proxy_fence {
   uint32_t flags;
   uint32_t seqno;
   uint64_t fence_id;
   struct list_head head;
};

static inline void
proxy_context_resource_add(struct proxy_context *ctx, uint32_t res_id)
{
   assert(!_mesa_hash_table_search(ctx->resource_table, (void *)(uintptr_t)res_id));
   _mesa_hash_table_insert(ctx->resource_table, (void *)(uintptr_t)res_id, NULL);
}

static inline bool
proxy_context_resource_find(struct proxy_context *ctx, uint32_t res_id)
{
   return _mesa_hash_table_search(ctx->resource_table, (void *)(uintptr_t)res_id);
}

static inline void
proxy_context_resource_remove(struct proxy_context *ctx, uint32_t res_id)
{
   _mesa_hash_table_remove_key(ctx->resource_table, (void *)(uintptr_t)res_id);
}

static inline bool
proxy_context_resource_table_init(struct proxy_context *ctx)
{
   ctx->resource_table = _mesa_hash_table_create_u32_keys(NULL);
   ctx->status_resource_table = _mesa_hash_table_create_u32_keys(NULL);
   return ctx->resource_table && ctx->status_resource_table;
}

static inline void
proxy_context_resource_table_fini(struct proxy_context *ctx)
{
   _mesa_hash_table_destroy(ctx->resource_table, NULL);
   _mesa_hash_table_destroy(ctx->status_resource_table, NULL);
}

static inline bool
proxy_context_status_resource_find(struct proxy_context *ctx, uint32_t res_id)
{
   return _mesa_hash_table_search(ctx->status_resource_table, (void *)(uintptr_t)res_id);
}

static bool
proxy_fence_is_signaled(const struct proxy_fence *fence, uint32_t cur_seqno)
{
   /* takes wrapping into account */
   const uint32_t d = cur_seqno - fence->seqno;
   return d < INT32_MAX;
}

static struct proxy_fence *
proxy_context_alloc_fence(struct proxy_context *ctx)
{
   struct proxy_fence *fence = NULL;

   if (proxy_renderer.flags & VIRGL_RENDERER_ASYNC_FENCE_CB)
      mtx_lock(&ctx->free_fences_mutex);

   if (!list_is_empty(&ctx->free_fences)) {
      fence = list_first_entry(&ctx->free_fences, struct proxy_fence, head);
      list_del(&fence->head);
   }

   if (proxy_renderer.flags & VIRGL_RENDERER_ASYNC_FENCE_CB)
      mtx_unlock(&ctx->free_fences_mutex);

   return fence ? fence : malloc(sizeof(*fence));
}

static void
proxy_context_free_fence(struct proxy_context *ctx, struct proxy_fence *fence)
{
   if (proxy_renderer.flags & VIRGL_RENDERER_ASYNC_FENCE_CB)
      mtx_lock(&ctx->free_fences_mutex);

   list_add(&fence->head, &ctx->free_fences);

   if (proxy_renderer.flags & VIRGL_RENDERER_ASYNC_FENCE_CB)
      mtx_unlock(&ctx->free_fences_mutex);
}

static uint32_t
proxy_context_load_timeline_seqno(struct proxy_context *ctx, uint32_t ring_idx)
{
   return atomic_load(&ctx->timeline_seqnos[ring_idx]);
}

static bool
proxy_context_retire_timeline_fences_locked(struct proxy_context *ctx,
                                            uint32_t ring_idx,
                                            uint32_t cur_seqno)
{
   struct proxy_timeline *timeline = &ctx->timelines[ring_idx];
   /* once worker exit was observed, retire every fence whose fd will
    * never signal instead of leaving a hung engine to the guest */
   const bool force_retire_all =
      atomic_load_explicit(&ctx->worker_dead, memory_order_acquire);

   if (timeline->cur_seqno == cur_seqno && !list_is_empty(&timeline->fences) &&
       !force_retire_all)
      return false;

   timeline->cur_seqno = cur_seqno;

   list_for_each_entry_safe (struct proxy_fence, fence, &timeline->fences, head) {
      if (!proxy_fence_is_signaled(fence, timeline->cur_seqno) && !force_retire_all)
         return false;

      struct render_ring_lat_rec *lat_rec = NULL;
      uint64_t lat_cb_begin = 0;
      if (ctx->lat) {
         lat_rec = render_ring_lat_rec(ctx->lat, fence->fence_id);
         if (atomic_load_explicit(&lat_rec->fence_id,
                                  memory_order_acquire) != fence->fence_id)
            lat_rec = NULL;
         if (lat_rec) {
            lat_rec->t_vmm_wake = ctx->lat_vmm_wake_ts;
            lat_cb_begin = render_ring_now_ns();
         }
      }

      ctx->base.fence_retire(&ctx->base, ring_idx, fence->fence_id);

      if (lat_rec) {
         lat_rec->t_vmm_cb_begin = lat_cb_begin;
         lat_rec->t_vmm_cb_end = render_ring_now_ns();
      }

      /* The fence is done; drop the fd submit_fence registered for it (see
       * virgl_fence_retire).  Retirement here is driven by the timeline, not
       * by the fd, so nothing else will ever drop it -- and force_retire_all
       * retires precisely the fences whose fd is never going to signal. */
      virgl_fence_retire(fence->fence_id);

      list_del(&fence->head);
      proxy_context_free_fence(ctx, fence);
   }

   return true;
}

static void
proxy_context_retire_fences_internal(struct proxy_context *ctx)
{
#if !defined(__APPLE__)
   if (ctx->sync_thread.fence_eventfd >= 0)
      flush_eventfd(ctx->sync_thread.fence_eventfd);
#endif

   if (proxy_renderer.flags & VIRGL_RENDERER_ASYNC_FENCE_CB)
      mtx_lock(&ctx->timeline_mutex);

   uint64_t new_busy_mask = 0;
   uint64_t old_busy_mask = ctx->timeline_busy_mask;
   while (old_busy_mask) {
      const uint32_t ring_idx = u_bit_scan64(&old_busy_mask);
      const uint32_t cur_seqno = proxy_context_load_timeline_seqno(ctx, ring_idx);
      if (!proxy_context_retire_timeline_fences_locked(ctx, ring_idx, cur_seqno))
         new_busy_mask |= 1ull << ring_idx;
   }

   ctx->timeline_busy_mask = new_busy_mask;

   if (proxy_renderer.flags & VIRGL_RENDERER_ASYNC_FENCE_CB)
      mtx_unlock(&ctx->timeline_mutex);
}

static bool
proxy_context_sync_has_work(void *opaque)
{
   struct proxy_context *ctx = opaque;
   uint64_t busy = atomic_load_explicit(&ctx->timeline_busy_mask,
                                        memory_order_acquire);
   while (busy) {
      const uint32_t ring_idx = u_bit_scan64(&busy);
      if (proxy_context_load_timeline_seqno(ctx, ring_idx) !=
          ctx->timelines[ring_idx].cur_seqno)
         return true;
   }
   return false;
}

/* The worker asks for a guest interrupt by setting irq_pending; the
 * sync thread clears it and forwards one config-change interrupt per
 * set.  Runs on every wake, not only on the completion ring. */
static void
proxy_context_poll_irq(struct proxy_context *ctx)
{
   struct npt_context_status_page *page =
      atomic_load_explicit(&ctx->status_page, memory_order_acquire);
   if (page && atomic_exchange(&page->irq_pending, 0) &&
       proxy_renderer.cbs->context_irq)
      proxy_renderer.cbs->context_irq(ctx->base.ctx_id);
}

static int
proxy_context_sync_thread(void *arg)
{
   struct proxy_context *ctx = arg;

   assert(proxy_renderer.flags & VIRGL_RENDERER_ASYNC_FENCE_CB);

   _Atomic uint64_t *vmm_sleeps =
      ctx->ring ? &ctx->ring->stats_vmm.v.vmm_sleeps : NULL;

   while (!ctx->sync_thread.stop) {
      proxy_context_poll_irq(ctx);
      proxy_context_retire_fences_internal(ctx);
      if (ctx->sync_thread.stop)
         break;

#if defined(__APPLE__)
      /* Wait only on the completion semaphore: the process-wide
       * EVFILT_PROC monitor sets worker_dead and signals us, so a dead
       * worker can no longer strand this thread and its pending fences. */
      const enum render_ring_wait_result res = render_ring_consumer_wait(
         &ctx->ring->vmm_state.value, ctx->worker.completion_wake,
         proxy_context_sync_has_work, ctx, 0 /* spin_ns */, -1 /* timeout */,
         vmm_sleeps);
      if (ctx->lat)
         ctx->lat_vmm_wake_ts = render_ring_now_ns();

      if (res == RENDER_RING_WAIT_ERROR) {
         proxy_log("failed to wait on completion semaphore");
         break;
      }

      /* the monitor callback already marked the status page lost and
       * stored worker_dead; only the force-retire pass is left */
      if (!ctx->worker_death_handled &&
          atomic_load_explicit(&ctx->worker_dead, memory_order_acquire)) {
         ctx->worker_death_handled = true;
         proxy_log("ctx %d worker died; retiring all fences",
                   ctx->base.ctx_id);
         proxy_context_retire_fences_internal(ctx);
      }
#else
      /* Poll the completion wake fd and the death pipe with no timeout:
       * the worker closes the pipe on exit, so a dead worker can no
       * longer strand this thread and its pending fences. */
      const int death_fd = ctx->death_read_fd;
      const enum render_ring_wait_result res = render_ring_consumer_wait_fd(
         &ctx->ring->vmm_state.value, ctx->sync_thread.fence_eventfd, death_fd,
         proxy_context_sync_has_work, ctx, 0 /* spin_ns */, -1 /* timeout */,
         vmm_sleeps);
      if (ctx->lat)
         ctx->lat_vmm_wake_ts = render_ring_now_ns();

      if (res == RENDER_RING_WAIT_ERROR) {
         proxy_log("failed to poll fence eventfd");
         break;
      }

      if (res == RENDER_RING_WAIT_FD1 && death_fd >= 0) {
         /* death pipe readable (EOF) or HUP: the worker is gone */
         char b;
         const ssize_t r = read(death_fd, &b, 1);
         if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
            proxy_log("ctx %d worker died; retiring all fences",
                      ctx->base.ctx_id);
            /* seq_cst: pairs with the submit_fence post-publish load, so
             * a fence registered after this store always sees it */
            atomic_store_explicit(&ctx->worker_dead, true,
                                  memory_order_seq_cst);
            struct npt_context_status_page *page =
               atomic_load_explicit(&ctx->status_page, memory_order_seq_cst);
            if (page)
               proxy_status_page_mark_lost(page, NPT_CONTEXT_STATUS_LOST_WORKER_EXIT,
                                           render_ring_now_ns());
            if (page && proxy_renderer.cbs->context_irq)
               proxy_renderer.cbs->context_irq(ctx->base.ctx_id);
            close(death_fd);
            ctx->death_read_fd = -1;
            proxy_context_retire_fences_internal(ctx);
         }
      }
#endif
   }

   return 0;
}

/* Out-of-band fence routing.  Only Neptune event-ring fences qualify:
 * their retirement pairs with an ARM that travels independently (the host
 * parks whichever side arrives first), so registering them ahead of queued
 * submits is legal -- and registering them promptly is the point, since in
 * the main ring a fence queues behind the worker's in-flight submits and
 * every present-gate wake lags that backlog.  Low rings retire in dispatch
 * order ("everything before me was consumed") and have to stay on the
 * ordered main ring.  The ring split mirrors the host's
 * NPT_EVENT_RING_BASE convention: the upper half of the timeline range. */
static bool
proxy_context_fence_ring_is_oob(const struct proxy_context *ctx,
                                uint32_t ring_idx)
{
   return ctx->has_oob_ring && ring_idx >= PROXY_CONTEXT_TIMELINE_COUNT / 2;
}

static void
proxy_context_ring_stats_log(struct proxy_context *ctx)
{
   const struct render_ring_stats_vmm *sv = &ctx->ring->stats_vmm.v;
   const struct render_ring_stats_worker *sw = &ctx->ring->stats_worker.v;
   proxy_log("ring stats ctx %u: main_entries=%llu main_wakes=%llu "
             "main_sleeps=%llu oob_entries=%llu oob_wakes=%llu "
             "oob_sleeps=%llu fence_notifies=%llu vmm_sleeps=%llu "
             "backpressure_waits=%llu socket_payload_cmds=%llu",
             ctx->base.ctx_id,
             (unsigned long long)atomic_load(&sv->main_entries),
             (unsigned long long)atomic_load(&sv->main_wakes),
             (unsigned long long)atomic_load(&sw->main_sleeps),
             (unsigned long long)atomic_load(&sv->oob_entries),
             (unsigned long long)atomic_load(&sv->oob_wakes),
             (unsigned long long)atomic_load(&sw->oob_sleeps),
             (unsigned long long)atomic_load(&sw->fence_notifies),
             (unsigned long long)atomic_load(&sv->vmm_sleeps),
             (unsigned long long)atomic_load(&sv->backpressure_waits),
             (unsigned long long)atomic_load(&sv->socket_payload_cmds));
}

static void
proxy_context_ring_stats_maybe_log(struct proxy_context *ctx, uint64_t entries)
{
   if (ctx->ring_stats_enabled && entries % 65536 == 0)
      proxy_context_ring_stats_log(ctx);
}

/* Bounded wait for ring space: spin/yield briefly, then nanosleep(20 us)
 * until the consumer catches up.  Returns false only when the worker is
 * dead -- producers must never hang once worker_dead is set.
 *
 * Caller holds ring_mutex. */
static bool
proxy_context_ring_push(struct proxy_context *ctx,
                        uint32_t op,
                        const void *payload,
                        uint32_t size)
{
   unsigned spins = 0;
   const struct timespec backoff = { .tv_sec = 0, .tv_nsec = 20000 };
   for (;;) {
      if (render_ring_push(&ctx->main_ring, op, payload, size)) {
         const uint64_t entries =
            atomic_fetch_add(&ctx->ring->stats_vmm.v.main_entries, 1) + 1;
         render_ring_producer_wake(&ctx->ring->main_state.value,
                                   proxy_context_main_wake(ctx),
                                   &ctx->ring->stats_vmm.v.main_wakes);
         proxy_context_ring_stats_maybe_log(ctx, entries);
         return true;
      }
      if (atomic_load_explicit(&ctx->worker_dead, memory_order_acquire))
         return false;
      atomic_fetch_add(&ctx->ring->stats_vmm.v.backpressure_waits, 1);
      if (spins++ < 100)
         sched_yield();
      else
         nanosleep(&backoff, NULL);
   }
}

static bool
proxy_context_ring_push_oob(struct proxy_context *ctx,
                            const struct render_ring_fence *fence)
{
   unsigned spins = 0;
   const struct timespec backoff = { .tv_sec = 0, .tv_nsec = 20000 };
   for (;;) {
      if (render_ring_oob_push(&ctx->oob_ring, fence)) {
         atomic_fetch_add(&ctx->ring->stats_vmm.v.oob_entries, 1);
         render_ring_producer_wake(&ctx->ring->oob_state.value,
                                   proxy_context_oob_wake(ctx),
                                   &ctx->ring->stats_vmm.v.oob_wakes);
         return true;
      }
      if (atomic_load_explicit(&ctx->worker_dead, memory_order_acquire))
         return false;
      atomic_fetch_add(&ctx->ring->stats_vmm.v.backpressure_waits, 1);
      if (spins++ < 100)
         sched_yield();
      else
         nanosleep(&backoff, NULL);
   }
}

/* Publish the (ring_idx, seqno) -> fence_id mapping the worker reads
 * back off the latency trace area.  Must run before the ring push:
 * the worker's acquire of the tail orders this write in. */
static void
proxy_context_lat_seqmap_publish(struct proxy_context *ctx,
                                 uint32_t ring_idx, uint32_t seqno,
                                 uint64_t fence_id)
{
   struct render_ring_lat_seqent *se =
      render_ring_lat_seqmap(ctx->lat, ring_idx, seqno);
   atomic_store_explicit(&se->seqno, seqno, memory_order_relaxed);
   atomic_store_explicit(&se->fence_id, fence_id, memory_order_release);
}

/* Initialize the latency record for a just-pushed fence.  Caller holds
 * ring_mutex so concurrent submit_fence calls cannot interleave record
 * writes; fence_id is stored last (release) so the worker only ever sees
 * a fully initialized record. */
static void
proxy_context_lat_submit(struct proxy_context *ctx, uint64_t fence_id,
                         uint32_t ring_idx, uint32_t path, uint64_t t_arrive)
{
   const uint32_t idx = (uint32_t)(fence_id & (RENDER_RING_LAT_RECORDS - 1));
   struct render_ring_lat_rec *rec = &ctx->lat->recs[idx];
   ctx->lat->ring_idx[idx] = ring_idx;
   ctx->lat->path[idx] = path;
   rec->t_arrive = t_arrive;
   rec->t_pushed = render_ring_now_ns();
   rec->t_worker_recv = 0;
   rec->t_worker_signal = 0;
   rec->t_vmm_wake = 0;
   rec->t_vmm_cb_begin = 0;
   rec->t_vmm_cb_end = 0;
   atomic_thread_fence(memory_order_seq_cst);
   atomic_store_explicit(&rec->fence_id, fence_id, memory_order_release);
}

static int
proxy_context_submit_fence(struct virgl_context *base,
                           uint32_t flags,
                           uint32_t ring_idx,
                           uint64_t fence_id)
{
   struct proxy_context *ctx = (struct proxy_context *)base;
   const uint64_t old_busy_mask = ctx->timeline_busy_mask;
   const uint64_t lat_t_arrive = ctx->lat ? render_ring_now_ns() : 0;

   if (ring_idx >= PROXY_CONTEXT_TIMELINE_COUNT)
      return -EINVAL;

   struct proxy_timeline *timeline = &ctx->timelines[ring_idx];
   struct proxy_fence *fence = proxy_context_alloc_fence(ctx);
   if (!fence)
      return -ENOMEM;

   fence->flags = flags;
   fence->seqno = timeline->next_seqno++;
   fence->fence_id = fence_id;

   if (proxy_renderer.flags & VIRGL_RENDERER_ASYNC_FENCE_CB)
      mtx_lock(&ctx->timeline_mutex);

   list_addtail(&fence->head, &timeline->fences);
   ctx->timeline_busy_mask |= 1ull << ring_idx;

   if (proxy_renderer.flags & VIRGL_RENDERER_ASYNC_FENCE_CB)
      mtx_unlock(&ctx->timeline_mutex);

   /* Event-ring fences take the out-of-band ring: registered by the
    * worker's dedicated oob thread instead of queueing behind the
    * dispatch loop's in-flight submits.  Any channel failure falls
    * through to the ordered main-ring path below. */
   const struct render_ring_fence rf = {
      .flags = flags,
      .ring_idx = ring_idx,
      .seqno = fence->seqno,
   };
   bool ok;
   uint32_t lat_path = 0;
   mtx_lock(&ctx->ring_mutex);
   if (ctx->lat)
      proxy_context_lat_seqmap_publish(ctx, ring_idx, fence->seqno,
                                       fence_id);
   if (proxy_context_fence_ring_is_oob(ctx, ring_idx)) {
      ok = proxy_context_ring_push_oob(ctx, &rf);
      lat_path = 1;
   } else {
      ok = proxy_context_ring_push(ctx, RENDER_RING_OP_SUBMIT_FENCE, &rf,
                                   sizeof(rf));
   }
   if (ok && ctx->lat)
      proxy_context_lat_submit(ctx, fence_id, ring_idx, lat_path,
                               lat_t_arrive);
   mtx_unlock(&ctx->ring_mutex);

   /* The push may have succeeded on ring space that was still free while
    * the worker died between this fence's registration and the sync
    * thread's force-retire pass.  Re-check with seq_cst (paired with the
    * sync thread's store before it takes timeline_mutex): if the worker
    * is dead, keep the fence registered and nudge the sync thread into
    * another retire pass -- the fence gets force-retired like the rest. */
   if (atomic_load_explicit(&ctx->worker_dead, memory_order_seq_cst)) {
      proxy_context_nudge_sync(ctx);
      return 0;
   }

   if (ok)
      return 0;

   /* recover timeline fences and busy_mask on submit_fence request failure */
   if (proxy_renderer.flags & VIRGL_RENDERER_ASYNC_FENCE_CB)
      mtx_lock(&ctx->timeline_mutex);

   list_del(&fence->head);
   ctx->timeline_busy_mask = old_busy_mask;

   if (proxy_renderer.flags & VIRGL_RENDERER_ASYNC_FENCE_CB)
      mtx_unlock(&ctx->timeline_mutex);

   proxy_context_free_fence(ctx, fence);
   proxy_log("failed to submit fence");
   return -1;
}

static void
proxy_context_retire_fences(struct virgl_context *base)
{
   struct proxy_context *ctx = (struct proxy_context *)base;

   assert(!(proxy_renderer.flags & VIRGL_RENDERER_ASYNC_FENCE_CB));
   proxy_context_retire_fences_internal(ctx);
}

static int
proxy_context_get_fencing_fd(struct virgl_context *base)
{
   struct proxy_context *ctx = (struct proxy_context *)base;

   assert(!(proxy_renderer.flags & VIRGL_RENDERER_ASYNC_FENCE_CB));
#if defined(__APPLE__)
   /* no fd-based completion channel on macOS: ASYNC_FENCE_CB is
    * required there (libkrun always sets it) */
   proxy_log("ctx %d: fencing fd unavailable without ASYNC_FENCE_CB",
             ctx->base.ctx_id);
   return -1;
#else
   return ctx->sync_thread.fence_eventfd;
#endif
}

static int
proxy_context_submit_cmd(struct virgl_context *base, const void *buffer, size_t size)
{
   struct proxy_context *ctx = (struct proxy_context *)base;

   if (!size)
      return 0;

   if (atomic_load_explicit(&ctx->worker_dead, memory_order_acquire))
      return -EIO;

   bool ok;
   mtx_lock(&ctx->ring_mutex);
   if (size <= RENDER_RING_MAX_INLINE_CMD) {
      ok = proxy_context_ring_push(ctx, RENDER_RING_OP_SUBMIT_CMD, buffer,
                                   (uint32_t)size);
   } else {
      /* oversized payload: publish the marker first, then the command
       * bytes follow on the context socket */
      const uint32_t wire_size = (uint32_t)size;
      ok = proxy_context_ring_push(ctx, RENDER_RING_OP_SUBMIT_CMD_SOCKET,
                                   &wire_size, sizeof(wire_size));
      if (ok) {
         ok = proxy_socket_send_request(&ctx->socket, buffer, size);
         atomic_fetch_add(&ctx->ring->stats_vmm.v.socket_payload_cmds, 1);
         if (!ok)
            proxy_log("failed to submit large cmd buffer");
      }
   }
   mtx_unlock(&ctx->ring_mutex);

   if (!ok) {
      proxy_log("failed to submit cmd");
      return -1;
   }

   return 0;
}

static bool
validate_resource_fd_shm(int fd, uint64_t expected_size)
{
#if defined(HAVE_MEMFD_CREATE)
   static const int blocked_seals = F_SEAL_WRITE;

   const int seals = fcntl(fd, F_GET_SEALS);
   if (seals & blocked_seals) {
      proxy_log("failed to validate shm seals(%d): blocked(%d)", seals, blocked_seals);
      return false;
   }
#endif

   struct stat st;
   if (fstat(fd, &st) < 0) {
      proxy_log("failed to fstat shm fd");
      return false;
   }
   if ((uint64_t)st.st_size < expected_size) {
      proxy_log("shm size(%" PRIu64 ") smaller than expected(%" PRIu64 ")",
                (uint64_t)st.st_size, expected_size);
      return false;
   }

   return true;
}

static inline int
add_required_seals_to_fd(int fd)
{
#if defined(HAVE_MEMFD_CREATE)
   return fcntl(fd, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW);
#else
   // if memfd_create is not available pretend file sealing worked
   (void)fd;
   return 0;
#endif
}

static struct npt_context_status_page *
proxy_context_status_page_create(struct proxy_context *ctx)
{
   int fd = os_create_anonymous_file(NPT_CONTEXT_STATUS_PAGE_SIZE, "npt-status");
   if (fd < 0)
      return NULL;
   if (add_required_seals_to_fd(fd)) {
      close(fd);
      return NULL;
   }

   void *ptr = mmap(NULL, NPT_CONTEXT_STATUS_PAGE_SIZE, PROT_READ | PROT_WRITE,
                    MAP_SHARED, fd, 0);
   if (ptr == MAP_FAILED) {
      close(fd);
      return NULL;
   }

   proxy_status_page_init(ptr);
   ctx->status_fd = fd;
   return ptr;
}

static int
proxy_context_get_status_blob(struct proxy_context *ctx, uint32_t res_id,
                              uint64_t blob_size, struct virgl_context_blob *blob)
{
   if (blob_size != NPT_CONTEXT_STATUS_PAGE_SIZE) {
      proxy_log("status blob size %" PRIu64 " for res %d", blob_size, res_id);
      return -1;
   }

   struct npt_context_status_page *page =
      atomic_load_explicit(&ctx->status_page, memory_order_acquire);
   if (!page) {
      page = proxy_context_status_page_create(ctx);
      if (!page) {
         proxy_log("failed to create status page for ctx %d", ctx->base.ctx_id);
         return -1;
      }
      atomic_store_explicit(&ctx->status_page, page, memory_order_seq_cst);
      /* pairs with the death path: either it sees this page, or we see its
       * worker_dead store */
      if (atomic_load_explicit(&ctx->worker_dead, memory_order_seq_cst))
         proxy_status_page_mark_lost(page, NPT_CONTEXT_STATUS_LOST_WORKER_EXIT,
                                     render_ring_now_ns());
      proxy_log("ctx %d: status page created (res %d, fd %d)", ctx->base.ctx_id,
                res_id, ctx->status_fd);
   }

   const int fd = dup(ctx->status_fd);
   if (fd < 0)
      return -1;

   blob->type = VIRGL_RESOURCE_FD_SHM;
   blob->u.fd = fd;
   blob->map_info = VIRGL_RENDERER_MAP_CACHE_CACHED;
   blob->export_format = 0;

   _mesa_hash_table_insert(ctx->status_resource_table, (void *)(uintptr_t)res_id, NULL);
   return 0;
}

static int
proxy_context_get_blob(struct virgl_context *base,
                       uint32_t res_id,
                       uint64_t blob_id,
                       uint64_t blob_size,
                       uint32_t blob_flags,
                       struct virgl_context_blob *blob)
{
   /* RENDER_CONTEXT_OP_CREATE_RESOURCE implies resource attach, thus proxy tracks
    * resources created here to avoid double attaching the same resource when proxy is on
    * attach_resource callback.
    */
   struct proxy_context *ctx = (struct proxy_context *)base;

   if (blob_id == NPT_CONTEXT_STATUS_BLOB_ID)
      return proxy_context_get_status_blob(ctx, res_id, blob_size, blob);

   const struct render_context_op_create_resource_request req = {
      .header.op = RENDER_CONTEXT_OP_CREATE_RESOURCE,
      .res_id = res_id,
      .blob_id = blob_id,
      .blob_size = blob_size,
      .blob_flags = blob_flags,
   };

   /* Cold ops keep fds and replies on the socket; the SOCKET_REQUEST
    * marker preserves their order relative to ring submits.  The mutex
    * is held across the whole transaction so concurrent producers can
    * neither reorder markers nor interleave socket bytes. */
   mtx_lock(&ctx->ring_mutex);
   bool ok = proxy_context_ring_push(ctx, RENDER_RING_OP_SOCKET_REQUEST, NULL, 0);
   if (ok)
      ok = proxy_socket_send_request(&ctx->socket, &req, sizeof(req));
   if (!ok) {
      mtx_unlock(&ctx->ring_mutex);
      proxy_log("failed to get blob %" PRIu64, blob_id);
      return -1;
   }

   struct render_context_op_create_resource_reply reply;
   int reply_fd;
   int reply_fd_count;
   ok = proxy_socket_receive_reply_with_fds(&ctx->socket, &reply, sizeof(reply),
                                            &reply_fd, 1, &reply_fd_count);
   mtx_unlock(&ctx->ring_mutex);
   if (!ok) {
      proxy_log("failed to get reply of blob %" PRIu64, blob_id);
      return -1;
   }

   if (!reply_fd_count) {
      proxy_log("invalid reply for blob %" PRIu64, blob_id);
      return -1;
   }

   bool reply_fd_valid = false;
   switch (reply.fd_type) {
   case VIRGL_RESOURCE_FD_DMABUF:
      /* TODO validate the fd is dmabuf >= blob_size */
      reply_fd_valid = true;
      break;
   case VIRGL_RESOURCE_FD_OPAQUE:
      /* this will be validated when imported by the client */
      reply_fd_valid = true;
      break;
   case VIRGL_RESOURCE_FD_SHM:
      /* validate the seals and size here */
      reply_fd_valid = !add_required_seals_to_fd(reply_fd) &&
                       validate_resource_fd_shm(reply_fd, blob_size);
      break;
   default:
      break;
   }
   if (!reply_fd_valid) {
      proxy_log("invalid fd type %d for blob %" PRIu64, reply.fd_type, blob_id);
      close(reply_fd);
      return -1;
   }

   blob->type = reply.fd_type;
   blob->u.fd = reply_fd;
   blob->map_info = reply.map_info;

   if (reply.fd_type == VIRGL_RESOURCE_FD_OPAQUE)
      blob->vulkan_info = reply.vulkan_info;

   blob->export_format = reply.export_format;

   proxy_context_resource_add(ctx, res_id);

   return 0;
}

static int
proxy_context_transfer_3d(struct virgl_context *base,
                          struct virgl_resource *res,
                          UNUSED const struct vrend_transfer_info *info,
                          UNUSED int transfer_mode)
{
   struct proxy_context *ctx = (struct proxy_context *)base;

   proxy_log("no transfer support for ctx %d and res %d", ctx->base.ctx_id, res->res_id);
   return -1;
}

static void
proxy_context_detach_resource(struct virgl_context *base, struct virgl_resource *res)
{
   struct proxy_context *ctx = (struct proxy_context *)base;
   const uint32_t res_id = res->res_id;

   /* avoid detaching resource not belonging to this context */
   if (!proxy_context_resource_find(ctx, res_id))
      return;

   const struct render_context_op_destroy_resource_request req = {
      .header.op = RENDER_CONTEXT_OP_DESTROY_RESOURCE,
      .res_id = res_id,
   };
   mtx_lock(&ctx->ring_mutex);
   bool ok = proxy_context_ring_push(ctx, RENDER_RING_OP_SOCKET_REQUEST, NULL, 0);
   if (ok)
      ok = proxy_socket_send_request(&ctx->socket, &req, sizeof(req));
   mtx_unlock(&ctx->ring_mutex);
   if (!ok)
      proxy_log("failed to detach res %d", res_id);

   proxy_context_resource_remove(ctx, res_id);
}

static void
proxy_context_attach_resource(struct virgl_context *base, struct virgl_resource *res)
{
   struct proxy_context *ctx = (struct proxy_context *)base;
   const uint32_t res_id = res->res_id;

   /* avoid importing resources created from RENDER_CONTEXT_OP_CREATE_RESOURCE */
   if (proxy_context_resource_find(ctx, res_id) ||
       proxy_context_status_resource_find(ctx, res_id))
      return;

   /* The current render protocol only supports importing dma-buf, shm or pipe resource
    * that can be exported to dma-buf. A protocol change is needed when there exists use
    * case for importing external Vulkan opaque resource.
    */
   if (res->fd_type != VIRGL_RESOURCE_FD_INVALID &&
       res->fd_type != VIRGL_RESOURCE_FD_DMABUF &&
       res->fd_type != VIRGL_RESOURCE_FD_SHM) {
      proxy_log("failed to attach res %d with fd_type %d", res_id, res->fd_type);
      return;
   }

   enum virgl_resource_fd_type res_fd_type = res->fd_type;
   int res_fd = res->fd;
   uint64_t res_size = res->map_size;
   bool close_res_fd = false;
   if (res_fd_type == VIRGL_RESOURCE_FD_INVALID) {
      /* importable pipe resouce can only export as dma-buf */
      res_fd_type = virgl_resource_export_fd(res, &res_fd);
      if (res_fd_type != VIRGL_RESOURCE_FD_DMABUF) {
         /* close fd for unexpected fd type from succeeded export */
         if (res_fd_type != VIRGL_RESOURCE_FD_INVALID)
            close(res_fd);
         proxy_log("exported res %d to unexpected fd_type %d", res_id, res_fd_type);
         return;
      }

      /* get the actual dma-buf size here because:
       * - pipe resource created by vrend has a zero map_size
       * - blob resource created by vrend can have non-zero fake map_size
       */
      res_size = lseek(res_fd, 0, SEEK_END);

      close_res_fd = true;
   }

   /* the proxy ignores iovs since transfer_3d is not supported */
   const struct render_context_op_import_resource_request req = {
      .header.op = RENDER_CONTEXT_OP_IMPORT_RESOURCE,
      .res_id = res_id,
      .fd_type = res_fd_type,
      .size = res_size,
      .export_format = res->export_format,
   };
   mtx_lock(&ctx->ring_mutex);
   bool ok = proxy_context_ring_push(ctx, RENDER_RING_OP_SOCKET_REQUEST, NULL, 0);
   if (ok)
      ok = proxy_socket_send_request_with_fds(&ctx->socket, &req, sizeof(req),
                                              &res_fd, 1);
   mtx_unlock(&ctx->ring_mutex);
   if (!ok)
      proxy_log("failed to attach res %d", res_id);

   if (res_fd >= 0 && close_res_fd)
      close(res_fd);

   proxy_context_resource_add(ctx, res_id);
}

struct proxy_lat_row {
   uint64_t fence_id;
   uint32_t ring_idx;
   uint32_t path;
   const struct render_ring_lat_rec *rec;
};

static int
proxy_lat_row_cmp(const void *a, const void *b)
{
   const struct proxy_lat_row *ra = a;
   const struct proxy_lat_row *rb = b;
   return (ra->fence_id > rb->fence_id) - (ra->fence_id < rb->fence_id);
}

static void
proxy_context_lat_dump(struct proxy_context *ctx)
{
   const struct render_ring_lat_area *lat = ctx->lat;

   uint32_t count = 0;
   for (uint32_t i = 0; i < RENDER_RING_LAT_RECORDS; i++)
      if (atomic_load_explicit(&lat->recs[i].fence_id,
                               memory_order_acquire) != 0)
         count++;
   if (!count)
      return;

   struct proxy_lat_row *rows = malloc(count * sizeof(*rows));
   if (!rows)
      return;
   uint32_t n = 0;
   for (uint32_t i = 0; i < RENDER_RING_LAT_RECORDS; i++) {
      const uint64_t fence_id = atomic_load_explicit(
         &lat->recs[i].fence_id, memory_order_acquire);
      if (!fence_id)
         continue;
      rows[n++] = (struct proxy_lat_row){
         .fence_id = fence_id,
         .ring_idx = lat->ring_idx[i],
         .path = lat->path[i],
         .rec = &lat->recs[i],
      };
   }
   qsort(rows, n, sizeof(*rows), proxy_lat_row_cmp);

   if (mkdir(ctx->lat_trace_dir, 0755) && errno != EEXIST) {
      proxy_log("latency trace: cannot create %s", ctx->lat_trace_dir);
      free(rows);
      return;
   }
   char path[4096];
   snprintf(path, sizeof(path), "%s/lat-ctx%u-%d.csv", ctx->lat_trace_dir,
            ctx->base.ctx_id, getpid());
   FILE *f = fopen(path, "w");
   if (!f) {
      proxy_log("latency trace: cannot write %s", path);
      free(rows);
      return;
   }
   fprintf(f, "fence_id,ring_idx,path,t_arrive,t_pushed,t_worker_recv,"
              "t_worker_signal,t_vmm_wake,t_vmm_cb_begin,t_vmm_cb_end\n");
   for (uint32_t i = 0; i < n; i++) {
      const struct render_ring_lat_rec *r = rows[i].rec;
      fprintf(f, "%llu,%u,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
              (unsigned long long)rows[i].fence_id, rows[i].ring_idx,
              rows[i].path,
              (unsigned long long)r->t_arrive,
              (unsigned long long)r->t_pushed,
              (unsigned long long)r->t_worker_recv,
              (unsigned long long)r->t_worker_signal,
              (unsigned long long)r->t_vmm_wake,
              (unsigned long long)r->t_vmm_cb_begin,
              (unsigned long long)r->t_vmm_cb_end);
   }
   fclose(f);
   proxy_log("latency trace: wrote %s (%u rows)", path, n);
   free(rows);
}

static void
proxy_context_destroy(struct virgl_context *base)
{
   struct proxy_context *ctx = (struct proxy_context *)base;

   /* Detach the MMIO doorbell slot first: a vCPU ringer holds the
    * slot's read lock while it touches the shared mask and the OOB
    * wake, so the slot must read empty before either is torn down. */
   proxy_doorbell_unregister(ctx->base.ctx_id);

   npt_work_context_client_destroy(ctx->work_context);

#if defined(__APPLE__)
   if (ctx->sync_thread.created) {
      ctx->sync_thread.stop = true;
      proxy_context_nudge_sync(ctx);
      thrd_join(ctx->sync_thread.thread, NULL);
   }
#else
   if (ctx->sync_thread.fence_eventfd >= 0) {
      if (ctx->sync_thread.created) {
         ctx->sync_thread.stop = true;
         write_eventfd(ctx->sync_thread.fence_eventfd_write, 1);
         thrd_join(ctx->sync_thread.thread, NULL);
      }

      if (ctx->sync_thread.fence_eventfd_write != ctx->sync_thread.fence_eventfd)
         close(ctx->sync_thread.fence_eventfd_write);
      close(ctx->sync_thread.fence_eventfd);
   }
#endif

   if (ctx->ring_stats_enabled && ctx->ring)
      proxy_context_ring_stats_log(ctx);

   if (ctx->lat)
      proxy_context_lat_dump(ctx);

#if !defined(__APPLE__)
   if (ctx->main_wake_write >= 0)
      close(ctx->main_wake_write);
   if (ctx->oob_wake_write >= 0)
      close(ctx->oob_wake_write);
   if (ctx->death_read_fd >= 0)
      close(ctx->death_read_fd);
   if (ctx->ring_fds.completion_write >= 0 &&
       ctx->ring_fds.completion_write != ctx->sync_thread.fence_eventfd_write)
      close(ctx->ring_fds.completion_write);
   if (ctx->ring_fds.main_wake_read >= 0)
      close(ctx->ring_fds.main_wake_read);
   if (ctx->ring_fds.death_write >= 0)
      close(ctx->ring_fds.death_write);
   if (ctx->ring_fds.oob_wake_read >= 0)
      close(ctx->ring_fds.oob_wake_read);
#endif
   proxy_socket_fini(&ctx->socket);

   /* terminate the context worker (on macOS: unwatch, reap, and release
    * its wake semaphores; elsewhere: ask the server process) */
   if (!proxy_client_destroy_context(ctx->client, ctx->base.ctx_id))
      proxy_log("failed to destroy ctx %d", ctx->base.ctx_id);

   if (ctx->shmem.ptr)
      munmap(ctx->shmem.ptr, ctx->shmem.size);
   if (ctx->shmem.fd >= 0)
      close(ctx->shmem.fd);

   struct npt_context_status_page *status_page =
      atomic_load_explicit(&ctx->status_page, memory_order_acquire);
   if (status_page)
      munmap(status_page, NPT_CONTEXT_STATUS_PAGE_SIZE);
   if (ctx->status_fd >= 0)
      close(ctx->status_fd);

   if (ctx->timeline_seqnos) {
      for (uint32_t i = 0; i < PROXY_CONTEXT_TIMELINE_COUNT; i++) {
         struct proxy_timeline *timeline = &ctx->timelines[i];
         list_for_each_entry_safe (struct proxy_fence, fence, &timeline->fences, head) {
            /* Never retired, so nothing dropped their registered fds. */
            virgl_fence_retire(fence->fence_id);
            free(fence);
         }
      }
   }
   mtx_destroy(&ctx->timeline_mutex);
   mtx_destroy(&ctx->ring_mutex);

   list_for_each_entry_safe (struct proxy_fence, fence, &ctx->free_fences, head)
      free(fence);
   mtx_destroy(&ctx->free_fences_mutex);

   proxy_context_resource_table_fini(ctx);

   free(ctx);
}

static void
proxy_context_init_base(struct proxy_context *ctx)
{
   ctx->base.destroy = proxy_context_destroy;
   ctx->base.attach_resource = proxy_context_attach_resource;
   ctx->base.detach_resource = proxy_context_detach_resource;
   ctx->base.transfer_3d = proxy_context_transfer_3d;
   ctx->base.get_blob = proxy_context_get_blob;
   ctx->base.submit_cmd = proxy_context_submit_cmd;

   ctx->base.get_fencing_fd = proxy_context_get_fencing_fd;
   ctx->base.retire_fences = proxy_context_retire_fences;
   ctx->base.submit_fence = proxy_context_submit_fence;
}

#if !defined(__APPLE__)
/* On eventfd platforms create_fence_pipe hands back one fd for both ends,
 * so the fd sent to the worker must be a dup: closing our own copy after
 * INIT would kill our end of the channel too.  When the ends differ the
 * fd itself travels. */
static int
proxy_context_worker_fd(int fd, int other_end)
{
   return fd == other_end ? dup(fd) : fd;
}
#endif

static bool
proxy_context_init_fencing(struct proxy_context *ctx)
{
#if !defined(__APPLE__)
   /* The render worker updates the shmem timelines and conditionally
    * signals the completion wake fd (always when the client lacks
    * VIRGL_RENDERER_ASYNC_FENCE_CB).  The main/oob wake pipes let the
    * proxy wake the worker's ring consumers, and the death pipe reports
    * worker exit to the sync thread.
    */
   if (create_fence_pipe(&ctx->sync_thread.fence_eventfd,
                         &ctx->sync_thread.fence_eventfd_write)) {
      proxy_log("failed to create fence eventfd/pipe");
      return false;
   }
   /* the worker gets its own completion-wake write end */
   ctx->ring_fds.completion_write =
      proxy_context_worker_fd(ctx->sync_thread.fence_eventfd_write,
                              ctx->sync_thread.fence_eventfd);

   int main_wake_read;
   if (create_fence_pipe(&main_wake_read, &ctx->main_wake_write)) {
      proxy_log("failed to create main ring wake pipe");
      return false;
   }
   ctx->ring_fds.main_wake_read =
      proxy_context_worker_fd(main_wake_read, ctx->main_wake_write);

   /* the death channel must be a real pipe on every platform: an eventfd
    * can never report EOF and would mask worker death entirely */
   int death_fds[2];
   if (pipe(death_fds)) {
      proxy_log("failed to create worker death pipe");
      return false;
   }
   for (int i = 0; i < 2; i++) {
      const int fdflags = fcntl(death_fds[i], F_GETFD);
      const int stflags = fcntl(death_fds[i], F_GETFL);
      if (fdflags < 0 || fcntl(death_fds[i], F_SETFD, fdflags | FD_CLOEXEC) ||
          stflags < 0 || fcntl(death_fds[i], F_SETFL, stflags | O_NONBLOCK)) {
         close(death_fds[0]);
         close(death_fds[1]);
         proxy_log("failed to set up worker death pipe");
         return false;
      }
   }
#ifdef F_SETNOSIGPIPE
   fcntl(death_fds[1], F_SETNOSIGPIPE, 1);
#endif
   ctx->death_read_fd = death_fds[0];
   ctx->ring_fds.death_write = death_fds[1];

   if (ctx->has_oob_ring) {
      int oob_wake_read;
      if (create_fence_pipe(&oob_wake_read, &ctx->oob_wake_write)) {
         proxy_log("failed to create oob ring wake pipe");
         return false;
      }
      ctx->ring_fds.oob_wake_read =
         proxy_context_worker_fd(oob_wake_read, ctx->oob_wake_write);
   }
#endif
   /* On macOS the three wake semaphores were created and registered as
    * the worker's ports in proxy_worker_spawn, and the EVFILT_PROC
    * monitor replaces the death pipe. */

   if (proxy_renderer.flags & VIRGL_RENDERER_ASYNC_FENCE_CB) {
      int ret = thrd_create(&ctx->sync_thread.thread, proxy_context_sync_thread, ctx);
      if (ret != thrd_success) {
         proxy_log("failed to create sync thread");
         return false;
      }
      ctx->sync_thread.created = true;
   }

   return true;
}

static bool
proxy_context_init_timelines(struct proxy_context *ctx)
{
   atomic_uint *timeline_seqnos = ctx->ring->timelines;
   for (uint32_t i = 0; i < ARRAY_SIZE(ctx->timelines); i++) {
      atomic_init(&timeline_seqnos[i], 0);

      struct proxy_timeline *timeline = &ctx->timelines[i];
      timeline->cur_seqno = 0;
      timeline->next_seqno = 1;
      list_inithead(&timeline->fences);
   }

   ctx->timeline_seqnos = timeline_seqnos;

   return true;
}

static int
alloc_memfd(const char *name, size_t size, void **out_ptr)
{
   int fd = os_create_anonymous_file(size, name);
   if (fd < 0)
      return -1;

   int ret = add_required_seals_to_fd(fd);
   if (ret)
      goto fail;

   if (!out_ptr)
      return fd;

   void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   if (ptr == MAP_FAILED)
      goto fail;

   *out_ptr = ptr;
   return fd;

fail:
   close(fd);
   return -1;
}

static bool
proxy_context_init_shmem(struct proxy_context *ctx)
{
   struct render_ring_layout layout;
   render_ring_layout_compute(&layout, ctx->has_oob_ring,
                              ctx->lat_trace_enabled);

   ctx->shmem.fd = alloc_memfd("proxy-ctx", layout.total_size, &ctx->shmem.ptr);
   if (ctx->shmem.fd < 0)
      return false;

   ctx->shmem.size = layout.total_size;
   ctx->ring = ctx->shmem.ptr;
   render_ring_control_init(ctx->ring);
   ctx->main_ring = render_ring_main_buf(ctx->ring, ctx->shmem.ptr,
                                         layout.main_offset, layout.main_size);
   if (ctx->has_oob_ring)
      ctx->oob_ring = render_ring_oob_buf(ctx->ring, ctx->shmem.ptr,
                                          layout.oob_offset, layout.oob_size);
   ctx->lat = render_ring_lat_area(ctx->shmem.ptr, layout.lat_offset);

   return true;
}

static bool
proxy_context_init(struct proxy_context *ctx, uint32_t ctx_flags)
{
   if (!proxy_context_init_shmem(ctx) || !proxy_context_init_timelines(ctx) ||
       !proxy_context_init_fencing(ctx) || !proxy_context_resource_table_init(ctx)) {
      proxy_log("failed to pre-initialize context");
      return false;
   }

   /* Neptune contexts get their status page eagerly: the completion
    * queue lives there, the worker needs the fd in INIT, and the
    * guest-visible blob must export this same shm. */
   if (ctx->has_oob_ring) {
      struct npt_context_status_page *page =
         proxy_context_status_page_create(ctx);
      if (!page) {
         proxy_log("failed to create status page for ctx %d",
                   ctx->base.ctx_id);
         return false;
      }
      atomic_store_explicit(&ctx->status_page, page, memory_order_seq_cst);
      /* pairs with the death path: either it sees this page, or we see
       * its worker_dead store */
      if (atomic_load_explicit(&ctx->worker_dead, memory_order_seq_cst))
         proxy_status_page_mark_lost(page, NPT_CONTEXT_STATUS_LOST_WORKER_EXIT,
                                     render_ring_now_ns());
   }

   struct render_ring_layout layout;
   render_ring_layout_compute(&layout, ctx->has_oob_ring,
                              ctx->lat_trace_enabled);

   /* Without VIRGL_RENDERER_ASYNC_FENCE_CB the client polls the
    * completion fd itself, so the worker must signal every update. */
   uint32_t ring_flags =
      (proxy_renderer.flags & VIRGL_RENDERER_ASYNC_FENCE_CB)
         ? 0
         : RENDER_RING_FLAG_ALWAYS_NOTIFY;
   if (ctx->lat_trace_enabled)
      ring_flags |= RENDER_RING_FLAG_LATENCY_TRACE;

   const struct render_context_op_init_request req = {
      .header.op = RENDER_CONTEXT_OP_INIT,
      .flags = ctx_flags,
      .ring_flags = ring_flags,
      .shmem_size = ctx->shmem.size,
      .main_ring_offset = layout.main_offset,
      .main_ring_size = layout.main_size,
      .oob_ring_offset = layout.oob_offset,
      .oob_ring_size = layout.oob_size,
      .lat_ring_offset = layout.lat_offset,
      .lat_ring_size = layout.lat_size,
      .work_context = ctx->work_context_config,
   };
#if defined(__APPLE__)
   /* INIT carries the ring shmem fd plus (Neptune only) the status
    * page fd: the wake semaphores were handed over at spawn as
    * registered ports and there is no death pipe. */
   const int req_fds[2] = { ctx->shmem.fd, ctx->status_fd };
   const int req_fd_count = ctx->has_oob_ring ? 2 : 1;
#else
   int req_fds[6];
   int req_fd_count = 0;
   req_fds[req_fd_count++] = ctx->shmem.fd;
   req_fds[req_fd_count++] = ctx->ring_fds.completion_write;
   req_fds[req_fd_count++] = ctx->ring_fds.main_wake_read;
   req_fds[req_fd_count++] = ctx->ring_fds.death_write;
   if (ctx->has_oob_ring)
      req_fds[req_fd_count++] = ctx->ring_fds.oob_wake_read;
   /* the status page fd travels last when the context has one */
   if (ctx->status_fd >= 0)
      req_fds[req_fd_count++] = ctx->status_fd;
#endif
   if (!proxy_socket_send_request_with_fds(&ctx->socket, &req, sizeof(req), req_fds,
                                           req_fd_count)) {
      proxy_log("failed to initialize context");
      return false;
   }

#if !defined(__APPLE__)
   /* the worker holds its own copies now; drop the ones we must not keep
    * (the death pipe reports EOF exactly when the worker exits; a dup
    * close leaves our own end intact on eventfd platforms) */
   if (ctx->ring_fds.completion_write != ctx->sync_thread.fence_eventfd_write)
      close(ctx->ring_fds.completion_write);
   ctx->ring_fds.completion_write = -1;
   close(ctx->ring_fds.main_wake_read);
   close(ctx->ring_fds.death_write);
   ctx->ring_fds.main_wake_read = -1;
   ctx->ring_fds.death_write = -1;
   if (ctx->ring_fds.oob_wake_read >= 0) {
      close(ctx->ring_fds.oob_wake_read);
      ctx->ring_fds.oob_wake_read = -1;
   }
#endif

   return true;
}

struct virgl_context *
proxy_context_create(uint32_t ctx_id,
                     uint32_t ctx_flags,
                     size_t debug_len,
                     const char *debug_name)
{
   struct proxy_client *client = proxy_renderer.client;
   struct proxy_context *ctx;

   const uint32_t capset_id = ctx_flags & VIRGL_RENDERER_CONTEXT_FLAG_CAPSET_ID_MASK;

   ctx = calloc(1, sizeof(*ctx));
   if (!ctx)
      return NULL;

#if defined(__APPLE__)
   /* the monitor must be able to flag the context before spawn returns */
   ctx->worker.on_exit = proxy_context_on_worker_exit;
   ctx->worker.udata = ctx;
#endif

   int ctx_fd;
   uint32_t worker_pid;
   if (!proxy_client_create_context(client, ctx_id, capset_id, debug_len, debug_name,
                                    &ctx_fd, &worker_pid,
#if defined(__APPLE__)
                                    &ctx->worker
#else
                                    NULL
#endif
                                    )) {
      proxy_log("failed to create a context");
      free(ctx);
      return NULL;
   }

   proxy_context_init_base(ctx);
   ctx->client = client;
   proxy_socket_init(&ctx->socket, ctx_fd);
   ctx->capset_id = capset_id;
   ctx->shmem.fd = -1;
   ctx->status_fd = -1;
   mtx_init(&ctx->timeline_mutex, mtx_plain);
   mtx_init(&ctx->ring_mutex, mtx_plain);
   mtx_init(&ctx->free_fences_mutex, mtx_plain);
   list_inithead(&ctx->free_fences);
   ctx->has_oob_ring = capset_id == VIRTGPU_DRM_CAPSET_NEPTUNE;
#if !defined(__APPLE__)
   ctx->sync_thread.fence_eventfd = -1;
   ctx->sync_thread.fence_eventfd_write = -1;
   ctx->main_wake_write = -1;
   ctx->oob_wake_write = -1;
   ctx->death_read_fd = -1;
   ctx->ring_fds.completion_write = -1;
   ctx->ring_fds.main_wake_read = -1;
   ctx->ring_fds.death_write = -1;
   ctx->ring_fds.oob_wake_read = -1;
#endif
   ctx->ring_stats_enabled = getenv("VIRGL_PROXY_RING_STATS") != NULL;
   ctx->lat_trace_dir = getenv("VIRGL_LATENCY_TRACE");
   ctx->lat_trace_enabled =
      ctx->lat_trace_dir && ctx->lat_trace_dir[0];

   if (capset_id == VIRTGPU_DRM_CAPSET_NEPTUNE)
      ctx->work_context = npt_work_context_client_create(ctx_id, worker_pid,
                                                       &ctx->work_context_config);

   if (!proxy_context_init(ctx, ctx_flags)) {
      proxy_context_destroy(&ctx->base);
      return NULL;
   }

   /* Expose the context's ring-shm doorbell mask and OOB wake to the
    * MMIO doorbell slot (Neptune only; ctx_ids >= slot count get no
    * doorbell). */
   if (ctx->has_oob_ring && ctx_id < PROXY_DOORBELL_SLOT_COUNT)
      proxy_doorbell_register(ctx_id, ctx->ring, proxy_context_oob_wake(ctx));

   return &ctx->base;
}
