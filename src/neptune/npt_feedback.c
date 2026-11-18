/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Per-context shared-memory feedback: substrate plus per-type
 * lifecycle (query, fence).  See npt_feedback.h for design notes.
 */

#include "npt_feedback.h"

#include <errno.h>
#include <inttypes.h>
#include <fcntl.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "util/hash_table.h"

#include "npt_com.h"
#include "npt_common.h"
#include "npt_context.h"
#include "npt_profile.h"
#include "npt_renderer.h"
#include "neptune-protocol/npt_protocol_transport.h"
#include "virgl_resource.h"

#include "neptune-protocol/npt_protocol_host_dispatch_types.h"
#include "neptune-protocol/npt_protocol_host_id3d11fence.h"
#include "neptune-protocol/npt_protocol_host_id3d12fence.h"

/* ================================================================== */
/* Substrate                                                           */
/* ================================================================== */

/* obj_id is uint64_t. */
static uint32_t
fb_hash_u64(const void *key)
{
   uint64_t v = *(const uint64_t *)key;
   v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9ull;
   v = (v ^ (v >> 27)) * 0x94d049bb133111ebull;
   return (uint32_t)(v ^ (v >> 31));
}

static bool
fb_equal_u64(const void *a, const void *b)
{
   return *(const uint64_t *)a == *(const uint64_t *)b;
}

static int npt_feedback_watcher_run(void *arg);
static void npt_feedback_watch_poke(struct npt_context *ctx);
static void npt_feedback_fence_unwatch_locked(struct npt_context *ctx,
                                              struct npt_feedback_entry *entry);
static void npt_feedback_fence_release_arm_locked(
   struct npt_context *ctx, struct npt_feedback_entry *entry);
static bool npt_feedback_fence_ensure_arm_locked(
   struct npt_context *ctx, struct npt_feedback_entry *entry);

void
npt_feedback_init(struct npt_context *ctx)
{
   struct npt_feedback_state *st = &ctx->feedback;
   memset(st, 0, sizeof(*st));
   mtx_init(&st->mutex, mtx_plain);
   mtx_init(&st->watch_mtx, mtx_plain);
   list_inithead(&st->pending);
   list_inithead(&st->watches);
   st->watch_ctl[0] = -1;
   st->watch_ctl[1] = -1;
   st->table = _mesa_hash_table_create(NULL, fb_hash_u64, fb_equal_u64);

   /* Control pipe: the watcher rebuilds its poll set (or exits) only
    * when poked, so binds and unregisters must reach it promptly. */
   int fds[2];
   if (pipe(fds) == 0) {
      for (int i = 0; i < 2; i++) {
         const int fl = fcntl(fds[i], F_GETFL);
         if (fl >= 0)
            fcntl(fds[i], F_SETFL, fl | O_NONBLOCK);
         const int cl = fcntl(fds[i], F_GETFD);
         if (cl >= 0)
            fcntl(fds[i], F_SETFD, cl | FD_CLOEXEC);
      }
      st->watch_ctl[0] = fds[0];
      st->watch_ctl[1] = fds[1];
   }
   if (st->watch_ctl[0] < 0) {
      npt_log("feedback: watcher control pipe failed; fence arms disabled");
      return;
   }
   st->watcher_started =
      thrd_create(&st->watcher, npt_feedback_watcher_run, ctx) == thrd_success;
   if (!st->watcher_started) {
      close(st->watch_ctl[0]);
      close(st->watch_ctl[1]);
      st->watch_ctl[0] = st->watch_ctl[1] = -1;
      npt_log("feedback: watcher thread failed; fence arms disabled");
   }
}

void
npt_feedback_fini(struct npt_context *ctx)
{
   struct npt_feedback_state *st = &ctx->feedback;
   if (!st->table)
      return;

   /* Stop the watcher first: it must not refresh an entry after the
    * table is gone.  With the thread joined the watcher is no longer
    * the sole fd closer, so leftover records are freed inline. */
   if (st->watcher_started) {
      mtx_lock(&st->watch_mtx);
      st->watcher_stop = true;
      mtx_unlock(&st->watch_mtx);
      npt_feedback_watch_poke(ctx);
      thrd_join(st->watcher, NULL);
      st->watcher_started = false;
   }
   if (st->watch_ctl[0] >= 0) {
      close(st->watch_ctl[0]);
      close(st->watch_ctl[1]);
      st->watch_ctl[0] = st->watch_ctl[1] = -1;
   }
   list_for_each_entry_safe(struct npt_feedback_watch, w, &st->watches,
                            head) {
      close(w->fd);
      list_del(&w->head);
      free(w);
   }
   mtx_destroy(&st->watch_mtx);

   mtx_lock(&st->mutex);
   hash_table_foreach(st->table, e) {
      struct npt_feedback_entry *entry = e->data;
      npt_feedback_fence_release_arm_locked(ctx, entry);
      free(entry);
   }
   _mesa_hash_table_destroy(st->table, NULL);
   st->table = NULL;
   list_inithead(&st->pending);
   st->pending_count = 0;
   mtx_unlock(&st->mutex);
   if (st->pinned_events)
      npt_log("feedback: %u arm event(s) pinned for late "
              "SetEventOnCompletion fire", st->pinned_events);
   mtx_destroy(&st->mutex);
}

/* Wake the watcher so it rebuilds its poll set or observes the stop
 * flag.  Non-blocking; a full pipe already means a wakeup is pending. */
static void
npt_feedback_watch_poke(struct npt_context *ctx)
{
   struct npt_feedback_state *st = &ctx->feedback;
   if (st->watch_ctl[1] < 0)
      return;
   const uint8_t b = 1;
   ssize_t n;
   do {
      n = write(st->watch_ctl[1], &b, sizeof(b));
   } while (n < 0 && errno == EINTR);
}

void
npt_feedback_state_lock(struct npt_context *ctx)
{
   mtx_lock(&ctx->feedback.mutex);
}

void
npt_feedback_state_unlock(struct npt_context *ctx)
{
   mtx_unlock(&ctx->feedback.mutex);
}

struct npt_feedback_entry *
npt_feedback_lookup_locked(struct npt_context *ctx, uint64_t obj_id)
{
   struct npt_feedback_state *st = &ctx->feedback;
   struct hash_entry *e = _mesa_hash_table_search(st->table, &obj_id);
   return e ? e->data : NULL;
}

struct npt_feedback_entry *
npt_feedback_lookup_by_host_obj_locked(struct npt_context *ctx,
                                       void *host_obj,
                                       uint8_t type)
{
   struct npt_feedback_state *st = &ctx->feedback;
   if (!st->table || !host_obj)
      return NULL;

   hash_table_foreach(st->table, e) {
      struct npt_feedback_entry *entry = e->data;
      if (entry->type == type && entry->host_obj == host_obj)
         return entry;
   }
   return NULL;
}

void
npt_feedback_set_pending_locked(struct npt_context *ctx,
                                struct npt_feedback_entry *entry)
{
   struct npt_feedback_state *st = &ctx->feedback;
   if (entry->pending)
      return;
   list_addtail(&entry->pending_head, &st->pending);
   entry->pending = true;
   st->pending_count++;
}

void
npt_feedback_clear_pending_locked(struct npt_context *ctx,
                                  struct npt_feedback_entry *entry)
{
   struct npt_feedback_state *st = &ctx->feedback;
   if (!entry->pending)
      return;
   list_del(&entry->pending_head);
   entry->pending = false;
   st->pending_count--;
}

struct npt_feedback_entry *
npt_feedback_register_locked(struct npt_context *ctx,
                             uint64_t obj_id,
                             uint8_t type,
                             uint32_t fb_res_id,
                             uint32_t fb_offset,
                             uint32_t slot_size)
{
   if (!ctx || !obj_id || !fb_res_id || !slot_size)
      return NULL;

   struct npt_feedback_state *st = &ctx->feedback;
   if (!st->table)
      return NULL;

   mtx_lock(&st->mutex);
   struct npt_feedback_entry *entry = npt_feedback_lookup_locked(ctx, obj_id);
   if (entry) {
      /* Re-register: drop pending state and refresh slot info.
       * Clear type-specific fields so a stale resolve from the
       * previous registration can't leak through. */
      npt_feedback_clear_pending_locked(ctx, entry);
      entry->type = type;
      entry->fb_res_id = fb_res_id;
      entry->fb_offset = fb_offset;
      entry->slot_size = slot_size;
      entry->host_obj = NULL;
      entry->host_ctx = NULL;
      entry->cookie = 0;
      entry->version = 0;
      /* A reused fence entry keeps its arm event and watch rather
       * than releasing and re-creating them: an outstanding arm's
       * late fire is just an early refresh.  armed_value resets only
       * when no registration can still be in flight; keeping it makes
       * a stale fire refresh the new lifecycle's slot harmlessly. */
      if (entry->published_high >= entry->armed_value)
         entry->armed_value = 0;
      entry->first_queue = 0;
      entry->target_value = 0;
      entry->published_high = 0;
      entry->persistent = false;
      entry->rewound = false;
      entry->live = false;
      /* Keep mutex held: caller sets type-specific fields then calls
       * npt_feedback_state_unlock. */
      return entry;
   }

   entry = calloc(1, sizeof(*entry));
   if (!entry) {
      mtx_unlock(&st->mutex);
      npt_log("feedback: OOM registering obj 0x%" PRIx64 " type=%u",
              obj_id, (unsigned)type);
      return NULL;
   }
   entry->obj_id = obj_id;
   entry->type = type;
   entry->fb_res_id = fb_res_id;
   entry->fb_offset = fb_offset;
   entry->slot_size = slot_size;
   list_inithead(&entry->pending_head);
   _mesa_hash_table_insert(st->table, &entry->obj_id, entry);
   /* Keep mutex held: caller sets type-specific fields. */
   return entry;
}

void
npt_feedback_unregister(struct npt_context *ctx, uint64_t obj_id)
{
   if (!ctx || !obj_id)
      return;
   struct npt_feedback_state *st = &ctx->feedback;
   if (!st->table)
      return;

   mtx_lock(&st->mutex);
   struct hash_entry *e = _mesa_hash_table_search(st->table, &obj_id);
   if (e) {
      struct npt_feedback_entry *entry = e->data;
      npt_feedback_clear_pending_locked(ctx, entry);
      npt_feedback_fence_unwatch_locked(ctx, entry);
      npt_feedback_fence_release_arm_locked(ctx, entry);
      _mesa_hash_table_remove(st->table, e);
      free(entry);
   }
   mtx_unlock(&st->mutex);
}

void *
npt_feedback_slot_ptr(struct npt_resource *res, uint32_t offset, uint32_t size)
{
   if (!res || res->fd_type != VIRGL_RESOURCE_FD_SHM || !res->u.data)
      return NULL;
   if ((size_t)offset + size > res->size)
      return NULL;
   return res->u.data + offset;
}

/* Forward decls for the type dispatch below. */
static bool
npt_feedback_query_poll_one(struct npt_context *ctx,
                            struct npt_feedback_entry *entry);

void
npt_feedback_poll(struct npt_context *ctx, uint32_t *skip)
{
   if (!ctx)
      return;
   /* Amortize the deadline check; see NPT_FEEDBACK_POLL_CHECK_EVERY. */
   if (*skip) {
      (*skip)--;
      return;
   }
   *skip = NPT_FEEDBACK_POLL_CHECK_EVERY - 1;

   npt_feedback_poll_interval(ctx, NPT_FEEDBACK_POLL_INTERVAL_NS);
}

void
npt_feedback_poll_interval(struct npt_context *ctx, uint64_t min_interval_ns)
{
   if (!ctx)
      return;
   struct npt_feedback_state *st = &ctx->feedback;
   if (!st->table)
      return;

   if (atomic_load_explicit((_Atomic uint32_t *)&st->pending_count,
                            memory_order_relaxed) == 0)
      return;

   const uint64_t now = npt_profile_now_ns();
   if (now - st->last_poll_ns < min_interval_ns)
      return;

   mtx_lock(&st->mutex);
   if (st->pending_count == 0) {
      st->last_poll_ns = now;
      mtx_unlock(&st->mutex);
      return;
   }

   list_for_each_entry_safe(struct npt_feedback_entry, entry,
                            &st->pending, pending_head) {
      bool remove = false;
      switch (entry->type) {
      case NPT_FEEDBACK_TYPE_QUERY:
         remove = npt_feedback_query_poll_one(ctx, entry);
         break;
      default:
         /* Unknown type: drop from pending so we don't spin; the
          * entry itself stays for unregister to free. */
         remove = true;
         break;
      }
      if (remove)
         npt_feedback_clear_pending_locked(ctx, entry);
   }
   const uint64_t end_ns = npt_profile_now_ns();
   st->total_poll_ns += end_ns - now;
   st->poll_count++;
   st->last_poll_ns = end_ns;
   mtx_unlock(&st->mutex);
}

/* ================================================================== */
/* Query feedback                                                      */
/* ================================================================== */

/* D3D11 has no GPU-side equivalent of vkCmdCopyQueryPoolResults, so
 * the dispatch thread polls GetData on the CPU between commands.
 * Single-threaded dispatch preserves FIFO ordering. */

/* Caller holds feedback state lock. */
static bool
npt_feedback_query_poll_one(struct npt_context *ctx,
                            struct npt_feedback_entry *entry)
{
   if (!entry->host_ctx || !entry->host_obj)
      return false;
   if (!entry->fb_res_id || !entry->cookie /* query_data_size */)
      return false;
   if (entry->cookie > NPT_QUERY_FEEDBACK_SLOT_RESULT)
      return false;

   struct npt_resource *res = npt_context_get_resource(ctx, entry->fb_res_id);
   struct npt_query_feedback_slot *slot =
      npt_feedback_slot_ptr(res, entry->fb_offset, sizeof(*slot));
   if (!slot)
      return false;

   /* PIPELINE_STATISTICS is 88 bytes; the slot caps result room.  Zeroed:
    * the host library writes an EVENT result as one byte, and the guest
    * reads the BOOL it asked for. */
   uint8_t scratch[NPT_QUERY_FEEDBACK_SLOT_RESULT] = {0};
   /* D3D11_ASYNC_GETDATA_DONOTFLUSH: D3D11 forbids implicit Flush
    * from inside another method's body, and the host driver crashes
    * if it happens mid-CopySubresourceRegion. */
   PFN_ID3D11DeviceContext_GetData get_data =
      NPT_COM_VTBL_FUNC(PFN_ID3D11DeviceContext_GetData,
                        npt_com_vtable(entry->host_ctx),
                        NPT_VTBL_ID3D11DeviceContext_GetData);
   HRESULT hr = get_data(entry->host_ctx, entry->host_obj,
                         scratch, entry->cookie,
                         /*D3D11_ASYNC_GETDATA_DONOTFLUSH=*/1);
   if (hr != 0 /* S_OK */)
      return false;

   memcpy(slot->result, scratch, entry->cookie);
   /* Release pairs with the guest's acquire-load on state.  Packing
    * (version, flag) into one 64-bit atomic gives an atomic snapshot. */
   const uint64_t state =
      ((uint64_t)entry->version << 32) | NPT_QUERY_FEEDBACK_FLAG_READY;
   atomic_store_explicit(&slot->state, state, memory_order_release);
   return true;
}

void
npt_feedback_query_register(struct npt_context *ctx,
                            uint64_t query_id,
                            uint32_t fb_res_id,
                            uint32_t fb_offset,
                            uint32_t query_data_size)
{
   if (!ctx || !query_id || !fb_res_id ||
       query_data_size > NPT_QUERY_FEEDBACK_SLOT_RESULT)
      return;

   struct npt_feedback_entry *entry = npt_feedback_register_locked(
      ctx, query_id, NPT_FEEDBACK_TYPE_QUERY, fb_res_id, fb_offset,
      sizeof(struct npt_query_feedback_slot));
   if (!entry)
      return;
   /* Mutex held by register_locked. */
   entry->cookie = query_data_size;
   entry->version = 0;
   entry->persistent = false;
   npt_feedback_state_unlock(ctx);
}

/* The context the poll calls GetData on.  End may arrive on a deferred
 * context, but GetData is only valid on an immediate context and returns
 * no result on a deferred one, so a deferred context resolves to its
 * device's immediate context.  The immediate context lives as long as
 * the device, like the raw host_ctx pointer kept here. */
static void *
npt_feedback_query_poll_ctx(void *host_ctx)
{
   PFN_ID3D11DeviceContext_GetType get_type =
      NPT_COM_VTBL_FUNC(PFN_ID3D11DeviceContext_GetType,
                        npt_com_vtable(host_ctx),
                        NPT_VTBL_ID3D11DeviceContext_GetType);
   if (get_type(host_ctx) != D3D11_DEVICE_CONTEXT_DEFERRED)
      return host_ctx;

   ID3D11Device *device = NULL;
   PFN_ID3D11DeviceChild_GetDevice get_dev =
      NPT_COM_VTBL_FUNC(PFN_ID3D11DeviceChild_GetDevice,
                        npt_com_vtable(host_ctx),
                        NPT_VTBL_ID3D11DeviceChild_GetDevice);
   get_dev(host_ctx, &device);
   if (!device)
      return NULL;
   ID3D11DeviceContext *imm = NULL;
   PFN_ID3D11Device_GetImmediateContext get_imm =
      NPT_COM_VTBL_FUNC(PFN_ID3D11Device_GetImmediateContext,
                        npt_com_vtable(device),
                        NPT_VTBL_ID3D11Device_GetImmediateContext);
   get_imm(device, &imm);
   if (imm)
      npt_com_release(imm);
   npt_com_release(device);
   return imm;
}

void
npt_feedback_query_mark_end(struct npt_context *ctx,
                            void *host_ctx,
                            void *host_query)
{
   if (!ctx || !host_query)
      return;
   if (!ctx->feedback.table)
      return;

   npt_feedback_state_lock(ctx);
   /* Linear scan: registered query counts are small in practice. */
   struct npt_feedback_entry *match = NULL;
   hash_table_foreach(ctx->feedback.table, e) {
      struct npt_feedback_entry *entry = e->data;
      if (entry->type != NPT_FEEDBACK_TYPE_QUERY)
         continue;
      if (entry->host_obj == host_query) {
         match = entry;
         break;
      }
      /* Resolve host_obj from guest_id on first encounter. */
      if (!entry->host_obj) {
         entry->host_obj = npt_context_lookup_object(
            ctx, NULL, entry->obj_id, NPT_OBJECT_TYPE_IUNKNOWN);
         if (entry->host_obj == host_query) {
            match = entry;
            break;
         }
      }
   }
   if (match) {
      if (!match->host_ctx)
         match->host_ctx = npt_feedback_query_poll_ctx(host_ctx);
      /* The version counts Ends, in lockstep with the guest, which
       * bumps its own count before sending each End.  The poll stamps
       * this on the result, so a result for an earlier End can never
       * satisfy a guest GetData for a later one. */
      match->version++;
      npt_feedback_set_pending_locked(ctx, match);
   }
   npt_feedback_state_unlock(ctx);

   /* Only a ring thread polls, and it may be parked; tell it a poll is
    * owed.  After the state unlock: npt_ring_notify_feedback() takes
    * ring->mutex, which the poll path nests the other way round. */
   if (match)
      npt_context_notify_rings_feedback(ctx);
}

/* ================================================================== */
/* Fence feedback                                                      */
/* ================================================================== */

/* D3D11 has no ID3D11Fence::Signal; the lifecycle hook lives on
 * ID3D11DeviceContext4::Signal and calls npt_feedback_fence_mark_signal. */

static bool
npt_feedback_type_is_fence(uint8_t type)
{
   return type == NPT_FEEDBACK_TYPE_FENCE ||
          type == NPT_FEEDBACK_TYPE_FENCE12;
}

static void npt_feedback_publish_live_locked(struct npt_context *ctx,
                                             struct npt_feedback_entry *entry);

void
npt_feedback_fence_register(struct npt_context *ctx,
                            uint64_t fence_id,
                            uint32_t fb_res_id,
                            uint32_t fb_offset,
                            uint32_t fence_api)
{
   if (!ctx || !fence_id || !fb_res_id)
      return;

   uint8_t type;
   switch (fence_api) {
   case NPT_FENCE_FEEDBACK_API_D3D11:
      type = NPT_FEEDBACK_TYPE_FENCE;
      break;
   case NPT_FENCE_FEEDBACK_API_D3D12:
      type = NPT_FEEDBACK_TYPE_FENCE12;
      break;
   default:
      npt_log("fence feedback: unknown fence_api %u for obj 0x%" PRIx64,
              fence_api, fence_id);
      return;
   }

   struct npt_feedback_entry *entry = npt_feedback_register_locked(
      ctx, fence_id, type, fb_res_id, fb_offset,
      sizeof(struct npt_d3d11_fence_feedback_slot));
   if (!entry)
      return;
   /* Persistent: the entry stays for the fence's lifetime; each
    * Signal-driven poll updates the slot in place.  host_obj is
    * resolved lazily on first Signal. */
   entry->persistent = true;
   if (!entry->host_obj)
      entry->host_obj = npt_context_lookup_object(
         ctx, NULL, entry->obj_id, NPT_OBJECT_TYPE_IUNKNOWN);
   npt_feedback_publish_live_locked(ctx, entry);
   npt_feedback_state_unlock(ctx);
}

static void npt_feedback_publish_live_locked(struct npt_context *ctx,
                                             struct npt_feedback_entry *entry);

/* Caller holds state lock.  Resolve the fence feedback entry for a
 * host fence pointer: cached host_obj match first, then lazy bind
 * through the object table for entries not yet bound.  Linear scans;
 * fence counts are small. */
static struct npt_feedback_entry *
npt_feedback_fence_find_locked(struct npt_context *ctx, void *host_fence)
{
   /* Fast path: entry's host_obj already resolved (every Signal
    * after the first hits this).  Both fence types share one host
    * pointer namespace, so scan for either. */
   struct npt_feedback_entry *entry = NULL;
   hash_table_foreach(ctx->feedback.table, e) {
      struct npt_feedback_entry *cand = e->data;
      if (npt_feedback_type_is_fence(cand->type) &&
          cand->host_obj == host_fence) {
         entry = cand;
         break;
      }
   }
   if (!entry) {
      /* First-time resolve: fence registered before any Signal hasn't
       * had host_obj cached.  Walk fence entries with NULL host_obj
       * and bind via the object table. */
      hash_table_foreach(ctx->feedback.table, e) {
         struct npt_feedback_entry *cand = e->data;
         if (!npt_feedback_type_is_fence(cand->type) || cand->host_obj)
            continue;
         cand->host_obj = npt_context_lookup_object(
            ctx, NULL, cand->obj_id, NPT_OBJECT_TYPE_IUNKNOWN);
         npt_feedback_publish_live_locked(ctx, cand);
         if (cand->host_obj == host_fence) {
            entry = cand;
            break;
         }
      }
   }
   if (entry)
      npt_feedback_publish_live_locked(ctx, entry);
   return entry;
}

/* Caller holds state lock.  Latch the rewind flag for the same
 * reason as the guest's cross-queue CAS: once the completed value can
 * legally move backwards (a requested Signal below target, or a
 * second distinct queue), the slot can no longer be trusted.
 *
 * The arm event and its watch stay: a late fire just drains and
 * refreshes, which is harmless, and arming already stops because the
 * entry is now rewound.  Releasing the event here would pin it for
 * the process life whenever a registration is still outstanding, and
 * a re-registered fence would leak one event per rewind. */
static void
npt_feedback_fence_latch_rewound_locked(
   struct npt_context *ctx, struct npt_feedback_entry *entry)
{
   if (entry->rewound)
      return;
   entry->rewound = true;

   struct npt_resource *res = npt_context_get_resource(ctx, entry->fb_res_id);
   struct npt_d3d11_fence_feedback_slot *slot =
      npt_feedback_slot_ptr(res, entry->fb_offset, sizeof(*slot));
   if (slot)
      atomic_fetch_or_explicit(&slot->flags, NPT_FENCE_FEEDBACK_FLAG_REWOUND,
                               memory_order_release);

   npt_log("fence feedback: obj 0x%" PRIx64 " rewound; guest reads it "
           "over the wire from now on", entry->obj_id);
}

void
npt_feedback_fence_mark_signal(struct npt_context *ctx, void *host_fence,
                               uint64_t value, uintptr_t queue)
{
   if (!ctx || !host_fence || !ctx->feedback.table)
      return;

   void *sig_handle = NULL;
   uint8_t entry_type = 0;

   npt_feedback_state_lock(ctx);
   struct npt_feedback_entry *entry =
      npt_feedback_fence_find_locked(ctx, host_fence);
   if (entry && !entry->rewound) {
      /* Host-side rewind latch, matching the guest's first_queue CAS:
       * two queues signalling one fence can execute their Signals in
       * either order, so the completed value is no longer monotonic
       * and the slot must not be trusted. */
      if (queue) {
         uintptr_t expected = 0;
         const bool first = atomic_compare_exchange_strong(
            (_Atomic uintptr_t *)&entry->first_queue, &expected, queue);
         if (!first && expected != queue)
            npt_feedback_fence_latch_rewound_locked(ctx, entry);
      }
      /* Signals from different queues reach different ring threads and
       * can be dispatched in either order, so only ever move the target
       * forward -- but a request below it is itself proof of a rewind. */
      if (value < entry->target_value)
         npt_feedback_fence_latch_rewound_locked(ctx, entry);
      else if (value > entry->target_value)
         entry->target_value = value;

      /* Arm the entry's event for the new highest value.  CPU Signal
       * (queue == 0) already refreshes synchronously after the host
       * call, so arming is reserved for queue signals. */
      if (!entry->rewound && queue &&
          value > entry->armed_value &&
          npt_feedback_fence_ensure_arm_locked(ctx, entry)) {
         entry->armed_value = value;
         sig_handle = npt_event_fd_signal_handle(&entry->arm_event);
         entry_type = entry->type;
      }
   }
   npt_feedback_state_unlock(ctx);

   if (sig_handle) {
      /* Deliberately outside the state lock: D3DMetal fires the event
       * synchronously when the value is already reached, and the fire
       * path (watcher -> refresh) takes that lock. */
      HRESULT hr;
      if (entry_type == NPT_FEEDBACK_TYPE_FENCE12) {
         PFN_ID3D12Fence_SetEventOnCompletion seoc = NPT_COM_VTBL_FUNC(
            PFN_ID3D12Fence_SetEventOnCompletion,
            npt_com_vtable(host_fence),
            NPT_VTBL_ID3D12Fence_SetEventOnCompletion);
         hr = seoc(host_fence, value, (HANDLE)sig_handle);
      } else {
         PFN_ID3D11Fence_SetEventOnCompletion seoc = NPT_COM_VTBL_FUNC(
            PFN_ID3D11Fence_SetEventOnCompletion,
            npt_com_vtable(host_fence),
            NPT_VTBL_ID3D11Fence_SetEventOnCompletion);
         hr = seoc(host_fence, value, (HANDLE)sig_handle);
      }
      if (!NPT_SUCCEEDED(hr)) {
         npt_log("fence feedback: SetEventOnCompletion failed "
                 "(hr=0x%lx, value=%" PRIu64 "); refreshing once",
                 (unsigned long)hr, value);
         /* The arm is lost; publish what the fence holds now rather
          * than leave the slot stale until the next Signal. */
         npt_feedback_fence_refresh(ctx, host_fence);
      }
   }
}

/* Caller holds feedback state lock.  Host GetCompletedValue through
 * the vtable matching the entry's fence type. */
static UINT64
npt_feedback_fence_host_value(struct npt_feedback_entry *entry)
{
   if (entry->type == NPT_FEEDBACK_TYPE_FENCE12) {
      PFN_ID3D12Fence_GetCompletedValue get12 =
         NPT_COM_VTBL_FUNC(PFN_ID3D12Fence_GetCompletedValue,
                           npt_com_vtable(entry->host_obj),
                           NPT_VTBL_ID3D12Fence_GetCompletedValue);
      return get12(entry->host_obj);
   }
   PFN_ID3D11Fence_GetCompletedValue get11 =
      NPT_COM_VTBL_FUNC(PFN_ID3D11Fence_GetCompletedValue,
                        npt_com_vtable(entry->host_obj),
                        NPT_VTBL_ID3D11Fence_GetCompletedValue);
   return get11(entry->host_obj);
}

/* Caller holds state lock.  Publishes the host's current value and then
 * LIVE, so the guest only trusts the slot once it holds a real value.
 * Runs once per registration, when the host object is bound. */
static void
npt_feedback_publish_live_locked(struct npt_context *ctx,
                                 struct npt_feedback_entry *entry)
{
   if (entry->live || !entry->host_obj || !entry->fb_res_id)
      return;

   struct npt_resource *res = npt_context_get_resource(ctx, entry->fb_res_id);
   struct npt_d3d11_fence_feedback_slot *slot =
      npt_feedback_slot_ptr(res, entry->fb_offset, sizeof(*slot));
   if (!slot)
      return;

   const UINT64 val = npt_feedback_fence_host_value(entry);
   atomic_store_explicit(&slot->completed_value, (uint64_t)val,
                         memory_order_release);
   entry->published_high = (uint64_t)val;
   entry->live = true;
   atomic_fetch_or_explicit(&slot->flags, NPT_FENCE_FEEDBACK_FLAG_LIVE,
                            memory_order_release);
}

void
npt_feedback_fence_refresh(struct npt_context *ctx, void *host_fence)
{
   if (!ctx || !host_fence || !ctx->feedback.table)
      return;

   npt_feedback_state_lock(ctx);
   struct npt_feedback_entry *entry =
      npt_feedback_fence_find_locked(ctx, host_fence);
   if (!entry || !entry->host_obj || !entry->fb_res_id) {
      npt_feedback_state_unlock(ctx);
      return;
   }

   struct npt_resource *res = npt_context_get_resource(ctx, entry->fb_res_id);
   struct npt_d3d11_fence_feedback_slot *slot =
      npt_feedback_slot_ptr(res, entry->fb_offset, sizeof(*slot));
   if (!slot) {
      npt_feedback_state_unlock(ctx);
      return;
   }

   /* Same snapshot discipline as the poller: plain release store of
    * the authoritative value, then the REWOUND flag after it so the
    * guest's flags-first read can never pair the flag with a value it
    * did not cover. */
   const UINT64 val = npt_feedback_fence_host_value(entry);
   atomic_store_explicit(&slot->completed_value, (uint64_t)val,
                         memory_order_release);

   if ((uint64_t)val < entry->published_high) {
      if (!entry->rewound) {
         entry->rewound = true;
         npt_log("fence feedback: obj 0x%" PRIx64 " rewound to %" PRIu64
                 " from %" PRIu64 "; guest reads it over the wire from now on",
                 entry->obj_id, (uint64_t)val, entry->published_high);
      }
      atomic_fetch_or_explicit(&slot->flags, NPT_FENCE_FEEDBACK_FLAG_REWOUND,
                               memory_order_release);
   } else {
      entry->published_high = (uint64_t)val;
   }
   npt_feedback_state_unlock(ctx);
}

/* ================================================================== */
/* Fence arm watcher                                                   */
/* ================================================================== */

/* A fence's completed value only changes when a Signal executes, so
 * instead of polling GetCompletedValue every 100 us the host arms one
 * event object per fence (the same kind the event-proxy path hands to
 * SetEventOnCompletion) and a single per-context watcher thread
 * refreshes the slot when an arm fires.  Lower arms firing early just
 * refresh early, which is harmless. */

/* Caller holds state lock.  Create the arm event and its watch record
 * on first use.  The watch fd is a non-blocking dup of the event's
 * pollable side; the record is owned by the watcher once linked. */
static bool
npt_feedback_fence_ensure_arm_locked(struct npt_context *ctx,
                                     struct npt_feedback_entry *entry)
{
   struct npt_feedback_state *st = &ctx->feedback;
   if (!st->watcher_started || !entry->host_obj)
      return false;

   if (!entry->arm_event_valid) {
      npt_event_fd_init_invalid(&entry->arm_event);
      if (npt_event_fd_create(&entry->arm_event) < 0) {
         npt_log("fence feedback: arm event create failed for obj 0x%"
                 PRIx64, entry->obj_id);
         return false;
      }
      entry->arm_event_valid = true;
   }
   if (entry->watch) {
      /* A reused entry may have bound a new host fence since the watch
       * was linked; the watcher refreshes by this pointer. */
      if (entry->watch->host_obj != entry->host_obj) {
         mtx_lock(&st->watch_mtx);
         entry->watch->host_obj = entry->host_obj;
         mtx_unlock(&st->watch_mtx);
      }
      return true;
   }

   const int fd = npt_event_fd_dup_wait_fd(&entry->arm_event);
   if (fd < 0) {
      npt_log("fence feedback: dup(arm wait fd) failed for obj 0x%"
              PRIx64, entry->obj_id);
      return false;
   }
   struct npt_feedback_watch *w = calloc(1, sizeof(*w));
   if (!w) {
      close(fd);
      return false;
   }
   w->fd = fd;
   w->host_obj = entry->host_obj;
   mtx_lock(&st->watch_mtx);
   list_addtail(&w->head, &st->watches);
   mtx_unlock(&st->watch_mtx);
   entry->watch = w;
   npt_feedback_watch_poke(ctx);
   return true;
}

/* Caller holds state lock.  The record's fd must outlive any poll the
 * watcher has in flight, so the watcher alone closes it: mark the
 * record dead and poke; it is closed and freed on the next rebuild. */
static void
npt_feedback_fence_unwatch_locked(struct npt_context *ctx,
                                  struct npt_feedback_entry *entry)
{
   struct npt_feedback_state *st = &ctx->feedback;
   if (!entry->watch)
      return;
   mtx_lock(&st->watch_mtx);
   entry->watch->dead = true;
   mtx_unlock(&st->watch_mtx);
   entry->watch = NULL;
   npt_feedback_watch_poke(ctx);
}

/* Caller holds state lock.  There is no way to retract a
 * SetEventOnCompletion, so the arm event is destroyed only when no
 * registration on it can still be signalled: it was never armed, or
 * every armed value is demonstrably consumed (the highest armed value
 * already published, which means every lower registration fired too).
 * Otherwise the event is pinned for the life of the process -- the
 * same contract npt_event_fini gives gate events: a late fire must
 * never hit a closed handle. */
static void
npt_feedback_fence_release_arm_locked(struct npt_context *ctx,
                                      struct npt_feedback_entry *entry)
{
   if (!entry->arm_event_valid)
      return;
   const bool consumed =
      !entry->armed_value || entry->published_high >= entry->armed_value;
   if (consumed) {
      npt_event_fd_destroy(&entry->arm_event);
   } else {
      ctx->feedback.pinned_events++;
      npt_log("fence feedback: pinning arm event for obj 0x%" PRIx64
              " (armed=%" PRIu64 " published=%" PRIu64
              "); a pending registration may still fire",
              entry->obj_id, entry->armed_value, entry->published_high);
   }
   entry->arm_event_valid = false;
   npt_event_fd_init_invalid(&entry->arm_event);
}

/* Per-context watcher: poll() every armed fence's wait fd plus the
 * control pipe; on a fire, drain the fd (the dup'd fds are
 * O_NONBLOCK, and a drained pipe-mirror token is the reset for the
 * manual-reset event, matching the gate path) and refresh the slot.
 * watch records marked dead are closed and freed here, so an in-flight
 * poll never sees its fd disappear. */
static int
npt_feedback_watcher_run(void *arg)
{
   struct npt_context *ctx = arg;
   struct npt_feedback_state *st = &ctx->feedback;

   for (;;) {
      mtx_lock(&st->watch_mtx);
      size_t n = 0;
      list_for_each_entry(struct npt_feedback_watch, w, &st->watches,
                          head) {
         if (!w->dead)
            n++;
      }
      struct pollfd *pfds = malloc((n + 1) * sizeof(*pfds));
      void **objs = malloc(n * sizeof(*objs));
      size_t cnt = 0;
      if (pfds && objs) {
         list_for_each_entry_safe(struct npt_feedback_watch, w,
                                  &st->watches, head) {
            if (w->dead) {
               close(w->fd);
               list_del(&w->head);
               free(w);
               continue;
            }
            pfds[cnt + 1].fd = w->fd;
            pfds[cnt + 1].events = POLLIN;
            pfds[cnt + 1].revents = 0;
            objs[cnt] = w->host_obj;
            cnt++;
         }
      }
      const bool stop = st->watcher_stop;
      mtx_unlock(&st->watch_mtx);

      if (!pfds || !objs) {
         free(pfds);
         free(objs);
         if (stop)
            break;
         /* Allocation failure: brief backoff, then rebuild. */
         const struct timespec ts = { 0, 1000000 };
         nanosleep(&ts, NULL);
         continue;
      }

      pfds[0].fd = st->watch_ctl[0];
      pfds[0].events = POLLIN;
      pfds[0].revents = 0;

      if (stop) {
         free(pfds);
         free(objs);
         break;
      }

      const int r = poll(pfds, cnt + 1, -1);
      if (r < 0 && errno != EINTR) {
         free(pfds);
         free(objs);
         continue;
      }
      if (r > 0 && (pfds[0].revents & POLLIN)) {
         uint8_t buf[64];
         ssize_t rd;
         do {
            rd = read(st->watch_ctl[0], buf, sizeof(buf));
         } while (rd > 0 || (rd < 0 && errno == EINTR));
      }
      for (size_t i = 0; i < cnt && r > 0; i++) {
         if (pfds[i + 1].revents & (POLLIN | POLLERR | POLLHUP)) {
            npt_event_fd_drain_wait_fd(pfds[i + 1].fd);
            /* host_obj is a lookup key only: an entry unregistered
             * since the rebuild simply misses the refresh. */
            npt_feedback_fence_refresh(ctx, objs[i]);
         }
      }
      free(pfds);
      free(objs);
   }
   return 0;
}
