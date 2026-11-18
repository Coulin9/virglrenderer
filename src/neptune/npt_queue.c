/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Per-ring sync-queue worker thread.  Polls a sync_file fd per
 * submitted fence and retires the fence when the host signals it.
 */

#include "npt_queue.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "npt_com.h"
#include "npt_context.h"
#include "npt_event.h"
#include "npt_feedback.h"
#include "npt_profile.h"
#include "util/u_thread.h"

#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#endif

/* The host D3D library gives the sync path no DEVICE_LOST signal -- only
 * an fd, and for gates a fence value -- so a wall-clock budget is the
 * only way to stop a wedged producer from trapping the guest forever.
 * It has to be wall clock rather than a count of poll returns: the
 * ring's gate event is shared by all of that ring's gates, so another
 * gate's fire wakes this poll arbitrarily often without any time
 * passing.  Healthy work finishes orders of magnitude inside it. */
#define NPT_QUEUE_DEVICE_LOST_SEC 30u

/* ----------------------------------------------------------------- */
/* sync alloc / free / retire                                          */
/* ----------------------------------------------------------------- */

/* Take every queued wakeup off the fd.  The event is level-triggered and
 * shared across the ring's gates, so a token left behind would wake the
 * next gate on a fire that was never meant for it.  The wait fd is
 * non-blocking (npt_event.c), so the loop ends on EAGAIN.  Returns
 * whether anything was actually there: a poll that reports readable and
 * then yields nothing is a dead fd (POLLERR/POLLHUP), not a fire. */
static bool
npt_queue_drain_wakeups(int fd)
{
   if (fd < 0)
      return false;

   bool drained = false;
   uint64_t token;
   ssize_t n;
   do {
      n = read(fd, &token, sizeof(token));
      drained |= n > 0;
   } while (n > 0 || (n < 0 && errno == EINTR));

   return drained;
}

static struct npt_queue_sync *
npt_queue_alloc_sync(uint32_t ring_idx,
                     uint64_t fence_id,
                     const struct npt_event_paired *paired)
{
   struct npt_queue_sync *sync = malloc(sizeof(*sync));
   if (!sync)
      return NULL;

   sync->sync_fd  = paired->fd;
   sync->ring_idx = ring_idx;
   sync->fence_id = fence_id;
   sync->release_proxy = paired->release_proxy;
   sync->check_fence = paired->check_fence;
   sync->check_value = paired->check_value;
   sync->queue_mode = paired->queue_mode;
   sync->fast_poll = false;
   sync->deadline_ns = npt_profile_now_ns() +
      (uint64_t)NPT_QUEUE_DEVICE_LOST_SEC * 1000000000ull;

   return sync;
}

static void
npt_queue_free_sync(struct npt_queue_sync *sync)
{
   if (sync->sync_fd >= 0)
      close(sync->sync_fd);
   free(sync);
}

/* `lost` marks a retirement that wasn't a genuine completion: the
 * device-lost deadline, a dead wait fd, worker death or teardown.  On
 * that path queue-mode entries must NOT publish their token (no
 * completion happened) -- the guest learns via the status page's lost
 * word; the entry only releases its proxies. */
static inline void
npt_queue_sync_retire(struct npt_queue *queue, struct npt_queue_sync *sync,
                      bool lost)
{
   /* Publish each armed fence's freshest completed value into its
    * feedback slot before the guest can observe the retirement. */
   if (sync->check_fence)
      npt_feedback_fence_refresh(queue->context, sync->check_fence);
   if (sync->release_proxy) {
      uint32_t count;
      void **fences =
         npt_event_take_seoc_fences(queue->context, sync->release_proxy, &count);
      for (uint32_t i = 0; i < count; i++) {
         npt_feedback_fence_refresh(queue->context, fences[i]);
         npt_com_release(fences[i]);
      }
      free(fences);
   }

   /* ARM_QUEUE retirement: the fence-value refresh above ran first;
    * now publish the token to the completion queue before dropping the
    * proxy, per the completion-path ordering. */
   if (sync->queue_mode && !lost)
      npt_event_queue_complete(queue->context, sync->release_proxy);

   /* Queue arms carry no guest fence (fence_id == 0): there is nothing
    * to retire back to the guest. */
   if (!sync->queue_mode)
      queue->context->retire_fence(queue->context->ctx_id,
                                 sync->ring_idx, sync->fence_id);
   /* The arm's proxy reference was transferred to this entry at
    * pairing; the fence has retired (the proxy fired), so the D3D
    * library is done writing the signal handle -- release it.  Queue
    * arms also drop the registration reference here: the guest sends
    * no RELEASE_EVENT for queue arms and the host retires the token
    * itself. */
   const uint64_t queue_token =
      (sync->queue_mode && sync->release_proxy)
         ? sync->release_proxy->token
         : 0;
   npt_event_release_proxy(queue->context, sync->release_proxy);
   if (sync->queue_mode) {
      npt_event_release_registration(queue->context, queue_token,
                                     sync->release_proxy);
      if (!lost)
         npt_event_queue_irq(queue->context);
   }
   if (sync->check_fence)
      npt_d3d12_gate_release(sync->check_fence);
   npt_queue_free_sync(sync);
}

/* ----------------------------------------------------------------- */
/* submit                                                              */
/* ----------------------------------------------------------------- */

bool
npt_queue_sync_submit(struct npt_queue *queue,
                      uint32_t ring_idx,
                      uint64_t fence_id,
                      const struct npt_event_paired *paired)
{
   struct npt_queue_sync *sync =
      npt_queue_alloc_sync(ring_idx, fence_id, paired);
   if (!sync) {
      if (paired->fd >= 0)
         close(paired->fd);
      npt_event_release_proxy(queue->context, paired->release_proxy);
      if (paired->check_fence)
         npt_d3d12_gate_release(paired->check_fence);
      return false;
   }

   /* Append to the queue's sync list and wake the worker. */
   mtx_lock(&queue->sync_thread.mutex);
   list_addtail(&sync->head, &queue->sync_thread.syncs);
   cnd_signal(&queue->sync_thread.cond);
   mtx_unlock(&queue->sync_thread.mutex);

   return true;
}

/* ----------------------------------------------------------------- */
/* worker thread                                                       */
/* ----------------------------------------------------------------- */

static int
npt_wait_sync_fd(int fd, int timeout_ms)
{
   if (fd < 0)
      return 0;

   struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
   int ret;
   do {
      ret = poll(&pfd, 1, timeout_ms);
   } while (ret < 0 && errno == EINTR);

   /* Treat POLLNVAL / POLLERR / POLLHUP and any other error as a
    * signal so the worker retires the fence rather than re-polling a
    * dead fd forever.  A concurrent close() of the sync fd can race a
    * poll already in flight; the sync queue is that fd's only
    * downstream and must not wedge. */
   if (ret > 0 && (pfd.revents & (POLLNVAL | POLLERR | POLLHUP)))
      return 1;
   if (ret < 0)
      return 1;

   return ret;
}

static int
npt_queue_thread(void *arg)
{
   struct npt_queue *queue = arg;
   struct npt_context *ctx = queue->context;
   char thread_name[16];
   snprintf(thread_name, sizeof(thread_name), "npt-queue-%u", ctx->ctx_id);
   u_thread_setname(thread_name);

#if defined(__APPLE__)
   /* Per-thread QoS (see npt_ring_thread for why nice is not usable on
    * Darwin).  This thread turns GPU completion into the guest's fence
    * wake, so its scheduling delay lands directly in every waiter's
    * latency. */
   pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif

   /* sync_thread.mutex protects the list; dropped across the poll,
    * reacquired before popping the entry. */
   const int kPollTimeoutMs = 3000;

   /* Value-gated (GATE_WAIT) syncs poll on a short period.  The wakeup
    * event is shared across the ring's gates, so this gate's fire can be
    * legally consumed by another gate's drain, leaving the value complete
    * with the event silent until the next timeout notices.  250 ms bounds
    * that latency at negligible idle cost, and the common path never
    * waits it out.  A gate that has already seen such a wake drops to
    * kGateFastPollMs. */
   const int kGatePollTimeoutMs = 250;
   const int kGateFastPollMs = 2;

   mtx_lock(&queue->sync_thread.mutex);
   while (true) {
      while (list_is_empty(&queue->sync_thread.syncs) && !queue->sync_thread.join) {
         if (!ctx->status_page ||
             !npt_completion_has_overflow(&ctx->evt_producer)) {
            cnd_wait(&queue->sync_thread.cond, &queue->sync_thread.mutex);
            continue;
         }
         mtx_unlock(&queue->sync_thread.mutex);
         if (npt_completion_flush(&ctx->evt_producer, ctx->status_page))
            npt_event_queue_irq(ctx);
         mtx_lock(&queue->sync_thread.mutex);
         if (!npt_completion_has_overflow(&ctx->evt_producer))
            continue;
         struct timespec ts;
         timespec_get(&ts, TIME_UTC);
         ts.tv_nsec += 1000000;
         if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
         }
         cnd_timedwait(&queue->sync_thread.cond, &queue->sync_thread.mutex, &ts);
      }

      if (queue->sync_thread.join)
         break;

      struct npt_queue_sync *sync =
         list_first_entry(&queue->sync_thread.syncs, struct npt_queue_sync, head);

      mtx_unlock(&queue->sync_thread.mutex);

      /* Overflowed completion tokens wait for guest head space; this
       * thread's poll cycle is the periodic per-context hook that
       * retries them, so shrink the wait to 1 ms while the FIFO is
       * non-empty. */
      if (npt_completion_has_overflow(&ctx->evt_producer) &&
          ctx->status_page &&
          npt_completion_flush(&ctx->evt_producer, ctx->status_page))
         npt_event_queue_irq(ctx);
      const bool ovf_pending =
         npt_completion_has_overflow(&ctx->evt_producer);

      struct timespec t0, t1;
      const bool trace = NPT_DEBUG(FENCE_TRACE);
      if (trace) {
         clock_gettime(CLOCK_MONOTONIC, &t0);
         t1 = t0;
      }
      int rc = 0;
      bool fired = false;

      if (sync->check_fence) {
         /* The event is only a wakeup source; the value is the truth.
          * Check it before waiting, since this gate's fire may already
          * have been consumed and polling first would then wait out a
          * full period while the value sits complete. */
         if (npt_d3d12_gate_reached(sync->check_fence, sync->check_value)) {
            npt_queue_drain_wakeups(sync->sync_fd);
            mtx_lock(&queue->sync_thread.mutex);
            goto retire;
         }

         int gate_timeout = sync->fast_poll ? kGateFastPollMs
                                            : kGatePollTimeoutMs;
         if (ovf_pending && gate_timeout > 1)
            gate_timeout = 1;
         rc = npt_wait_sync_fd(sync->sync_fd, gate_timeout);

         bool stale = false;
         if (rc > 0 && npt_queue_drain_wakeups(sync->sync_fd)) {
            stale = !npt_d3d12_gate_reached(sync->check_fence,
                                            sync->check_value);
         }

         mtx_lock(&queue->sync_thread.mutex);
         if (stale) {
            /* A fire arrived but the value is not ours: this gate's own
             * fire may already have been drained by an earlier one and
             * will never wake us. */
            sync->fast_poll = true;
         }
         if (npt_profile_now_ns() < sync->deadline_ns)
            continue;
         npt_log("queue %u fence_id=%" PRIu64 ": gate value %" PRIu64
                 " not reached in %us, retiring as device-lost",
                 queue->ring_idx, sync->fence_id, sync->check_value,
                 NPT_QUEUE_DEVICE_LOST_SEC);
         goto retire;
      }

      rc = npt_wait_sync_fd(sync->sync_fd,
                            ovf_pending ? 1 : kPollTimeoutMs);
      if (trace)
         clock_gettime(CLOCK_MONOTONIC, &t1);

      /* Consume the fire's token(s) like the gate path does: the proxy
       * event is manual-reset and nothing else drains its exported
       * pipe.  Each guest arm gets its own proxy, so a leftover token
       * can no longer retire a later fence; draining keeps the proxy's
       * descriptor quiet for whatever still polls it.  A readable fd
       * that yields no token is a dead fd (POLLERR/HUP or a racing
       * close), which for a queue arm is a loss, not a fire. */
      if (rc > 0)
         fired = npt_queue_drain_wakeups(sync->sync_fd);

      mtx_lock(&queue->sync_thread.mutex);

      if (rc == 0) {
         /* Poll timeout: keep waiting until the device-lost budget is
          * spent, then retire anyway so a wedged host (driver hang,
          * missing sync_file signal) doesn't trap the guest forever. */
         if (npt_profile_now_ns() < sync->deadline_ns)
            continue;
         npt_log("queue %u fence_id=%" PRIu64 ": no completion in %us, "
                 "retiring as device-lost", queue->ring_idx, sync->fence_id,
                 NPT_QUEUE_DEVICE_LOST_SEC);
      }
retire:;

      if (trace) {
         const int64_t dt_ns =
            (int64_t)(t1.tv_sec  - t0.tv_sec ) * 1000000000ll +
            (int64_t)(t1.tv_nsec - t0.tv_nsec);
         const uint64_t wait_us = dt_ns > 0 ? (uint64_t)(dt_ns / 1000) : 0;
         npt_profile_log_q_retire(queue->ring_idx, sync->fence_id,
                                  sync->sync_fd, wait_us, rc);
      }

      list_del(&sync->head);
      const bool lost = sync->queue_mode && (rc == 0 || !fired);
      npt_queue_sync_retire(queue, sync, lost);
   }
   mtx_unlock(&queue->sync_thread.mutex);

   return 0;
}

/* ----------------------------------------------------------------- */
/* thread init / fini                                                  */
/* ----------------------------------------------------------------- */

static int
npt_queue_sync_thread_init(struct npt_queue *queue)
{
   STATIC_ASSERT(thrd_success == 0);

   int ret = mtx_init(&queue->sync_thread.mutex, mtx_plain);
   if (ret != thrd_success)
      return ret;

   ret = cnd_init(&queue->sync_thread.cond);
   if (ret != thrd_success)
      goto fail_cnd_init;

   list_inithead(&queue->sync_thread.syncs);
   queue->sync_thread.join = false;

   ret = thrd_create(&queue->sync_thread.thread, npt_queue_thread, queue);
   if (ret != thrd_success)
      goto fail_thrd_create;

   return 0;

fail_thrd_create:
   cnd_destroy(&queue->sync_thread.cond);
fail_cnd_init:
   mtx_destroy(&queue->sync_thread.mutex);
   return ret;
}

static void
npt_queue_sync_thread_fini(struct npt_queue *queue)
{
   /* Signal exit and join. */
   mtx_lock(&queue->sync_thread.mutex);
   queue->sync_thread.join = true;
   cnd_signal(&queue->sync_thread.cond);
   mtx_unlock(&queue->sync_thread.mutex);

   thrd_join(queue->sync_thread.thread, NULL);

   /* Drain leftover syncs: free them and let the renderer treat
    * unretired fences as context-destroy. */
   unsigned leftover = 0;
   list_for_each_entry_safe (struct npt_queue_sync, sync,
                             &queue->sync_thread.syncs, head) {
      list_del(&sync->head);
      npt_queue_sync_retire(queue, sync, /*lost=*/true);
      leftover++;
   }
   if (leftover)
      npt_log("queue %u: drained %u leftover sync(s) on destroy",
              queue->ring_idx, leftover);

   mtx_destroy(&queue->sync_thread.mutex);
   cnd_destroy(&queue->sync_thread.cond);
}

/* ----------------------------------------------------------------- */
/* create / destroy                                                    */
/* ----------------------------------------------------------------- */

struct npt_queue *
npt_queue_create(struct npt_context *ctx, uint32_t ring_idx)
{
   /* Reject ring_idx values that would overflow ctx->sync_queues[].
    * Submit already bounds-checks, but creation has other callers; a
    * defensive check here prevents an orphan queue from arising on a
    * corrupt-command path. */
   if (ring_idx >= ARRAY_SIZE(ctx->sync_queues))
      return NULL;

   struct npt_queue *queue = calloc(1, sizeof(*queue));
   if (!queue)
      return NULL;

   queue->context  = ctx;
   queue->ring_idx = ring_idx;

   if (npt_queue_sync_thread_init(queue)) {
      free(queue);
      return NULL;
   }

   return queue;
}

void
npt_queue_destroy(struct npt_queue *queue)
{
   if (!queue)
      return;
   npt_queue_sync_thread_fini(queue);
   free(queue);
}
