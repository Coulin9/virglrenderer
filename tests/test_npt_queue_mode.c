/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Queue-mode arm test: ARM_QUEUE must not touch the parked-fence or
 * pending-arm lists, must publish its token on proxy fire without a
 * guest fence, and must never call retire_fence.  Links the real
 * npt_event.c / npt_queue.c / npt_completion.c against stubs for the
 * D3D backend, COM and the renderer.
 */

#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include "neptune/npt_context.h"
#include "neptune/npt_event.h"
#include "neptune/npt_queue.h"
#include "neptune/npt_library.h"
#include "neptune/npt_renderer.h"
#include "neptune/npt_profile.h"

#include "util/hash_table.h"
#include "util/list.h"

static int failures;

#define CHECK(cond, what)                                                \
   do {                                                                  \
      if (!(cond)) {                                                     \
         fprintf(stderr, "test_npt_queue_mode: FAIL %s\n", what);        \
         failures++;                                                     \
      }                                                                  \
   } while (0)

/* ---------------- stubs ---------------- */

uint32_t npt_debug_flags;
uint32_t npt_profile_period_ms;
struct npt_profile npt_profile;

void
npt_log(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   fputc('\n', stderr);
   va_end(ap);
}

uint64_t
npt_profile_now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void
npt_profile_log_q_retire(uint32_t ring_idx, uint64_t fence_id,
                         int sync_fd, uint64_t poll_us, int rc)
{
   (void)ring_idx; (void)fence_id; (void)sync_fd; (void)poll_us; (void)rc;
}

void
npt_feedback_fence_refresh(struct npt_context *ctx, void *host_fence)
{
   (void)ctx; (void)host_fence;
}

void
npt_d3d12_gate_addref(void *fence) { (void)fence; }
void
npt_d3d12_gate_release(void *fence) { (void)fence; }
bool
npt_d3d12_gate_seoc(void *fence, uint64_t value, void *signal_handle)
{
   (void)fence; (void)value; (void)signal_handle;
   return false;
}
bool
npt_d3d12_gate_reached(void *fence, uint64_t value)
{
   (void)fence; (void)value;
   return false;
}

/* Fake darwin backend event: a non-blocking pipe pair.  Handle is the
 * struct; signal writes one 8-byte token to the write end. */
struct fake_event {
   int rfd;
   int wfd;
};

static void *last_event;

static void *
fake_event_create(int manual_reset, int initial_state)
{
   (void)manual_reset; (void)initial_state;
   struct fake_event *e = calloc(1, sizeof(*e));
   int fds[2];
   if (!e || pipe(fds) != 0)
      return NULL;
   e->rfd = fds[0];
   e->wfd = fds[1];
   last_event = e;
   return e;
}

static void
fake_event_close(void *handle)
{
   struct fake_event *e = handle;
   if (!e)
      return;
   close(e->rfd);
   close(e->wfd);
   free(e);
}

static int
fake_event_dup_fd(void *handle)
{
   struct fake_event *e = handle;
   return e ? dup(e->rfd) : -1;
}

static void
fake_event_signal(void *handle)
{
   struct fake_event *e = handle;
   uint64_t t = 1;
   ssize_t n = write(e->wfd, &t, sizeof(t));
   (void)n;
}

/* The last wait fd a pairing handed to the sync queue.  On Linux the
 * events are eventfds and this fd is a writable dup of the same
 * counter; on darwin the backend handle (last_event) is signalled
 * instead. */
static int last_paired_fd = -1;

static void
test_fire_event(void)
{
#ifdef __APPLE__
   fake_event_signal(last_event);
#else
   uint64_t t = 1;
   ssize_t n = write(last_paired_fd, &t, sizeof(t));
   (void)n;
#endif
}

static struct npt_d3d_library fake_lib;

struct npt_d3d_library *
npt_renderer_get_library(void)
{
   return &fake_lib;
}

/* Minimal chained _mesa_hash_table (structs are public in the header). */
struct ht_node {
   struct hash_entry e;
   struct ht_node *next;
};

static struct ht_node **
ht_head(struct hash_table *ht)
{
   return (struct ht_node **)&ht->table;
}

struct hash_table *
_mesa_hash_table_create(void *mem_ctx,
                        uint32_t (*key_hash_function)(const void *key),
                        bool (*key_equals_function)(const void *a,
                                                    const void *b))
{
   (void)mem_ctx;
   struct hash_table *ht = calloc(1, sizeof(*ht));
   if (ht) {
      ht->key_hash_function = key_hash_function;
      ht->key_equals_function = key_equals_function;
   }
   return ht;
}

struct hash_entry *
_mesa_hash_table_insert_pre_hashed(struct hash_table *ht, uint32_t hash,
                                   const void *key, void *data)
{
   struct ht_node *n = calloc(1, sizeof(*n));
   if (!n)
      return NULL;
   n->e.hash = hash;
   n->e.key = key;
   n->e.data = data;
   n->next = *ht_head(ht);
   *ht_head(ht) = n;
   ht->entries++;
   return &n->e;
}

struct hash_entry *
_mesa_hash_table_search_pre_hashed(struct hash_table *ht, uint32_t hash,
                                   const void *key)
{
   for (struct ht_node *n = *ht_head(ht); n; n = n->next) {
      if (n->e.hash == hash && ht->key_equals_function(key, n->e.key))
         return &n->e;
   }
   return NULL;
}

void
_mesa_hash_table_remove(struct hash_table *ht, struct hash_entry *entry)
{
   struct ht_node **pp = ht_head(ht);
   while (*pp) {
      if (&(*pp)->e == entry) {
         struct ht_node *n = *pp;
         *pp = n->next;
         free(n);
         ht->entries--;
         return;
      }
      pp = &(*pp)->next;
   }
}

struct hash_entry *
_mesa_hash_table_next_entry(struct hash_table *ht,
                            struct hash_entry *entry)
{
   if (!entry)
      return *ht_head(ht) ? &(*ht_head(ht))->e : NULL;
   struct ht_node *n = (struct ht_node *)entry;
   return n->next ? &n->next->e : NULL;
}

void
_mesa_hash_table_destroy(struct hash_table *ht,
                         void (*delete_function)(struct hash_entry *entry))
{
   (void)delete_function;
   if (!ht)
      return;
   struct ht_node *n = *ht_head(ht);
   while (n) {
      struct ht_node *next = n->next;
      free(n);
      n = next;
   }
   free(ht);
}

/* Pair straight into the ring's sync queue, like the real
 * npt_context_pair_event_fence, minus queue-create contention. */
bool
npt_context_pair_event_fence(struct npt_context *ctx,
                             uint32_t ring_idx, uint64_t fence_id,
                             const struct npt_event_paired *paired)
{
   if (ring_idx >= ARRAY_SIZE(ctx->sync_queues))
      return false;
   last_paired_fd = paired->fd;
   if (!ctx->sync_queues[ring_idx])
      ctx->sync_queues[ring_idx] = npt_queue_create(ctx, ring_idx);
   if (!ctx->sync_queues[ring_idx]) {
      if (paired->fd >= 0)
         close(paired->fd);
      npt_event_release_proxy(ctx, paired->release_proxy);
      return false;
   }
   return npt_queue_sync_submit(ctx->sync_queues[ring_idx], ring_idx,
                              fence_id, paired);
}

/* ---------------- test ---------------- */

static _Atomic uint64_t retire_calls;
static uint64_t retire_last_id;
static int status_wakes;

static void
test_retire_fence(uint32_t ctx_id, uint32_t ring_idx, uint64_t fence_id)
{
   (void)ctx_id; (void)ring_idx;
   atomic_fetch_add(&retire_calls, 1);
   retire_last_id = fence_id;
}

static void
test_status_wake(void *opaque)
{
   (void)opaque;
   status_wakes++;
}

static bool
wait_for(volatile _Atomic uint64_t *addr, uint64_t want, int timeout_ms)
{
   for (int i = 0; i < timeout_ms; i++) {
      if (atomic_load_explicit(addr, memory_order_acquire) == want)
         return true;
      struct timespec ts = { 0, 1000000 };
      nanosleep(&ts, NULL);
   }
   return false;
}

int
main(void)
{
   fake_lib.pfn_event_create = fake_event_create;
   fake_lib.pfn_event_close = fake_event_close;
   fake_lib.pfn_event_dup_fd = fake_event_dup_fd;

   struct npt_context_status_page *page =
      mmap(NULL, NPT_CONTEXT_STATUS_PAGE_SIZE, PROT_READ | PROT_WRITE,
           MAP_SHARED | MAP_ANON, -1, 0);
   if (page == MAP_FAILED) {
      fprintf(stderr, "test_npt_queue_mode: mmap failed\n");
      return 1;
   }
   memset(page, 0, NPT_CONTEXT_STATUS_PAGE_SIZE);

   struct npt_context *ctx = calloc(1, sizeof(*ctx));
   if (!ctx)
      return 1;
   ctx->ctx_id = 7;
   ctx->retire_fence = test_retire_fence;
   ctx->status_page = page;
   ctx->status_wake = test_status_wake;
   if (!npt_event_init(ctx)) {
      fprintf(stderr, "test_npt_queue_mode: npt_event_init failed\n");
      return 1;
   }

   const uint32_t ring = NPT_EVENT_RING_BASE;

   /* A fence that outran its ARM parks on the ring. */
   struct npt_event_paired parked_out;
   int pr = npt_event_pop_arm_or_park_fence(ctx, ring, 555, &parked_out);
   CHECK(pr == NPT_EVENT_FENCE_PARKED, "fence parked ahead of arm");
   CHECK(!list_is_empty(&ctx->event_pending_fences), "park list non-empty");

   /* Queue arm: must not consume the parked fence, must not pend. */
   npt_event_register(ctx, 0xabc);
   CHECK(npt_event_arm_queue(ctx, 0xabc, ring), "arm_queue");
   CHECK(!list_is_empty(&ctx->event_pending_fences),
         "parked fence not consumed by queue arm");
   CHECK(list_is_empty(&ctx->event_pending_arms), "no pending arm");

   /* Fire the proxy: the sync entry publishes the token without any
    * guest fence, and retire_fence is never called. */
   test_fire_event();
   CHECK(wait_for(&page->evt_tail, 1, 5000),
         "queue arm published token on fire");
   CHECK(atomic_load_explicit(&page->evt_tokens[0],
                              memory_order_acquire) == 0xabc,
         "published token matches");
   CHECK(atomic_load(&retire_calls) == 0, "no guest fence retired for queue arm");

   /* Old path on a second ring: park a fence, ARM, pop pairs it,
    * fire retires the real fence id. */
   const uint32_t ring2 = NPT_EVENT_RING_BASE + 1;
   npt_event_register(ctx, 0xdef);
   CHECK(npt_event_arm(ctx, 0xdef, ring2), "arm");
   struct npt_event_paired out;
   int fd = npt_event_pop_arm_or_park_fence(ctx, ring2, 999, &out);
   CHECK(fd >= 0, "old-path arm popped");
   CHECK(npt_context_pair_event_fence(ctx, ring2, 999, &out),
         "old-path paired");
   test_fire_event();
   CHECK(wait_for(&retire_calls, 1, 5000),
         "old-path fence retired");
   CHECK(retire_last_id == 999, "old-path retired fence id");

   /* EVENT_QUEUE_KICK: publishes the sentinel token 0 and raises an
    * interrupt only when a guest waiter exists. */
   {
      const uint64_t tail =
         atomic_load_explicit(&page->evt_tail, memory_order_acquire);
      const int wakes_before = status_wakes;
      npt_event_queue_kick(ctx);
      CHECK(atomic_load_explicit(&page->evt_tokens[
                                    tail % NPT_COMPLETION_QUEUE_SIZE],
                                 memory_order_acquire) == 0,
            "kick publishes token 0");
      CHECK(atomic_load_explicit(&page->evt_tail,
                                 memory_order_acquire) == tail + 1,
            "kick advances tail");
      CHECK(status_wakes == wakes_before, "no irq without waiters");

      atomic_store(&page->guest_waiters, 1);
      npt_event_queue_kick(ctx);
      CHECK(atomic_load(&page->irq_pending) == 1, "kick latches irq_pending");
      CHECK(status_wakes == wakes_before + 1, "kick interrupts waiter");
      atomic_store(&page->guest_waiters, 0);
      atomic_store(&page->irq_pending, 0);
   }

   for (unsigned i = 0; i < ARRAY_SIZE(ctx->sync_queues); i++) {
      if (ctx->sync_queues[i]) {
         npt_queue_destroy(ctx->sync_queues[i]);
         ctx->sync_queues[i] = NULL;
      }
   }
   npt_event_fini(ctx);
   free(ctx);
   munmap(page, NPT_CONTEXT_STATUS_PAGE_SIZE);

   if (failures) {
      fprintf(stderr, "test_npt_queue_mode: %d check(s) failed\n",
              failures);
      return 1;
   }
   printf("test_npt_queue_mode: all checks passed\n");
   return 0;
}
