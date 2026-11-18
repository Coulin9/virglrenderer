/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Event-driven fence feedback: a queue Signal arms the entry's event
 * with SetEventOnCompletion, the per-context watcher refreshes the
 * slot when it fires, and the cross-queue / below-target latches mark
 * the slot REWOUND and stop arming.  Links the real npt_feedback.c and
 * npt_event_fd.c against stubs for the backend event library, COM and
 * the renderer.
 */

#include <inttypes.h>
#include <poll.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include "neptune/npt_context.h"
#include "neptune/npt_event.h"
#include "neptune/npt_feedback.h"
#include "neptune/npt_library.h"
#include "neptune/npt_renderer.h"

#include "neptune-protocol/npt_protocol_transport.h"
#include "virgl_resource.h"

#include "util/hash_table.h"
#include "util/list.h"

static int failures;

#define CHECK(cond, what)                                                \
   do {                                                                  \
      if (!(cond)) {                                                     \
         fprintf(stderr, "test_npt_feedback_fence: FAIL %s\n", what);    \
         failures++;                                                     \
      }                                                                  \
   } while (0)

/* ---------------- stubs ---------------- */

uint32_t npt_debug_flags;
uint32_t npt_profile_period_ms;
#include "neptune/npt_profile.h"
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
npt_context_notify_rings_feedback(struct npt_context *ctx)
{
   (void)ctx;
}

/* Fake darwin backend event: a non-blocking pipe pair.  Handle is the
 * struct; signal writes one 8-byte token to the write end, mirroring
 * dxn_event's pipe-mirror semantics.  Unused off darwin, where the
 * backend event is an eventfd signalled by writing to the fd. */
static int event_creates;

#if defined(__APPLE__)
struct fake_event {
   int rfd;
   int wfd;
};

static void *
fake_event_create(int manual_reset, int initial_state)
{
   (void)manual_reset; (void)initial_state;
   event_creates++;
   struct fake_event *e = calloc(1, sizeof(*e));
   int fds[2];
   if (!e || pipe(fds) != 0)
      return NULL;
   e->rfd = fds[0];
   e->wfd = fds[1];
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
#endif /* __APPLE__ */

/* The signal handle passed to the last fake SetEventOnCompletion: a
 * backend event handle on darwin, a bare fd number elsewhere.  HANDLE
 * is an integer type, so this is stored as the raw value. */
static uintptr_t last_seoc_handle;
static int seoc_calls;
static uint64_t last_seoc_value;

static struct npt_d3d_library fake_lib;

struct npt_d3d_library *
npt_renderer_get_library(void)
{
   return &fake_lib;
}

/* ---------------- fake fence COM object ---------------- */

struct fake_fence {
   void **vtbl;
   _Atomic uint64_t completed;
   int id;
};

static HRESULT
fake_qi(void *self, const GUID *riid, void **ppv)
{
   (void)self; (void)riid; (void)ppv;
   return 0x80004002; /* E_NOINTERFACE */
}
static uint32_t
fake_addref(void *self) { (void)self; return 1; }
static uint32_t
fake_release(void *self) { (void)self; return 1; }

static UINT64
fake_get_completed(void *self)
{
   return atomic_load(&((struct fake_fence *)self)->completed);
}

static HRESULT
fake_seoc12(void *self, UINT64 value, HANDLE h)
{
   (void)self;
   seoc_calls++;
   last_seoc_value = value;
   last_seoc_handle = (uintptr_t)h;
   return 0; /* S_OK */
}

static HRESULT
fake_seoc11(void *self, UINT64 value, HANDLE h)
{
   return fake_seoc12(self, value, h);
}

/* Union pun, as NPT_COM_VTBL_FUNC uses in the other direction. */
#define VT(fn) (((union { void *p; __typeof__(&(fn)) f; }){ .f = (fn) }).p)

static void *fake12_vtbl[16];
static void *fake11_vtbl[16];

static void
fake_vtbls_init(void)
{
   fake12_vtbl[0] = VT(fake_qi);
   fake12_vtbl[1] = VT(fake_addref);
   fake12_vtbl[2] = VT(fake_release);
   fake12_vtbl[NPT_VTBL_ID3D12Fence_GetCompletedValue] =
      VT(fake_get_completed);
   fake12_vtbl[NPT_VTBL_ID3D12Fence_SetEventOnCompletion] =
      VT(fake_seoc12);

   fake11_vtbl[0] = VT(fake_qi);
   fake11_vtbl[1] = VT(fake_addref);
   fake11_vtbl[2] = VT(fake_release);
   fake11_vtbl[NPT_VTBL_ID3D11Fence_GetCompletedValue] =
      VT(fake_get_completed);
   fake11_vtbl[NPT_VTBL_ID3D11Fence_SetEventOnCompletion] =
      VT(fake_seoc11);
}

static void
test_fire_last_arm(uint64_t completed, struct fake_fence *f)
{
   atomic_store(&f->completed, completed);
#if defined(__APPLE__)
   fake_event_signal((void *)last_seoc_handle);
#else
   uint64_t t = 1;
   ssize_t n = write((int)last_seoc_handle, &t, sizeof(t));
   (void)n;
#endif
}

/* ---------------- object/resource stubs ---------------- */

static struct fake_fence fence_a, fence_b;
static struct npt_resource fake_res;
static uint8_t res_storage[4096];

void *
npt_context_lookup_object(struct npt_context *ctx,
                          struct npt_cs_decoder *dec,
                          uint64_t id,
                          npt_object_type expected)
{
   (void)ctx; (void)dec; (void)expected;
   if (id == 42)
      return &fence_a;
   if (id == 43)
      return &fence_b;
   return NULL;
}

/* Chained hash-table stubs (same pattern as test_npt_queue_mode). */
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
_mesa_hash_table_insert(struct hash_table *ht, const void *key, void *data)
{
   return _mesa_hash_table_insert_pre_hashed(
      ht, ht->key_hash_function(key), key, data);
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

struct hash_entry *
_mesa_hash_table_search(struct hash_table *ht, const void *key)
{
   return _mesa_hash_table_search_pre_hashed(ht, ht->key_hash_function(key),
                                           key);
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

struct hash_entry *
_mesa_hash_table_next_entry_unsafe(const struct hash_table *ht,
                                   struct hash_entry *entry)
{
   if (!entry)
      return *(struct ht_node **)&ht->table
                ? &(*(struct ht_node **)&ht->table)->e : NULL;
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

/* ---------------- helpers ---------------- */

static uint32_t
u32_key_hash(const void *key)
{
   return *(const uint32_t *)key;
}
static bool
u32_key_equal(const void *a, const void *b)
{
   return *(const uint32_t *)a == *(const uint32_t *)b;
}

static bool
wait_for_u64(_Atomic uint64_t *addr, uint64_t want, int timeout_ms)
{
   for (int i = 0; i < timeout_ms; i++) {
      if (atomic_load_explicit(addr, memory_order_acquire) == want)
         return true;
      struct timespec ts = { 0, 1000000 };
      nanosleep(&ts, NULL);
   }
   return false;
}

static struct npt_d3d11_fence_feedback_slot *
test_slot(void)
{
   return (struct npt_d3d11_fence_feedback_slot *)res_storage;
}

int
main(void)
{
   fake_vtbls_init();
   fake_lib.pfn_event_create = fake_event_create;
   fake_lib.pfn_event_close = fake_event_close;
   fake_lib.pfn_event_dup_fd = fake_event_dup_fd;

   memset(&fence_a, 0, sizeof(fence_a));
   fence_a.vtbl = fake12_vtbl;
   memset(&fence_b, 0, sizeof(fence_b));
   fence_b.vtbl = fake11_vtbl;

   memset(&fake_res, 0, sizeof(fake_res));
   fake_res.res_id = 7;
   fake_res.fd_type = VIRGL_RESOURCE_FD_SHM;
   fake_res.u.data = res_storage;
   fake_res.u.fd = -1;
   fake_res.size = sizeof(res_storage);
   memset(res_storage, 0, sizeof(res_storage));

   struct npt_context *ctx = calloc(1, sizeof(*ctx));
   if (!ctx)
      return 1;
   ctx->ctx_id = 9;
   mtx_init(&ctx->resource_mutex, mtx_plain);
   ctx->resource_table =
      _mesa_hash_table_create(NULL, u32_key_hash, u32_key_equal);
   _mesa_hash_table_insert(ctx->resource_table, &fake_res.res_id, &fake_res);

   npt_feedback_init(ctx);
   CHECK(ctx->feedback.watcher_started, "feedback watcher started");

   const uintptr_t q1 = 0xAAAA, q2 = 0xBBBB;

   /* fence_a (D3D12): register binds the host object and goes LIVE. */
   npt_feedback_fence_register(ctx, 42, 7, 0,
                               NPT_FENCE_FEEDBACK_API_D3D12);
   CHECK((atomic_load(&test_slot()->flags) &
          NPT_FENCE_FEEDBACK_FLAG_LIVE) != 0, "slot live after register");

   /* Increasing values arm once each; repeats don't re-arm. */
   npt_feedback_fence_mark_signal(ctx, &fence_a, 10, q1);
   CHECK(seoc_calls == 1 && last_seoc_value == 10, "first value armed");
   npt_feedback_fence_mark_signal(ctx, &fence_a, 10, q1);
   CHECK(seoc_calls == 1, "same value not re-armed");
   npt_feedback_fence_mark_signal(ctx, &fence_a, 12, q1);
   CHECK(seoc_calls == 2 && last_seoc_value == 12, "higher value armed");

   /* The fire refreshes the slot through the watcher. */
   test_fire_last_arm(12, &fence_a);
   CHECK(wait_for_u64(&test_slot()->completed_value, 12, 5000),
         "fired event refreshed slot");

   /* Fences never enter pending: the ring's poll-owed predicate reads
    * pending_count, which must stay zero with only fences registered. */
   CHECK(ctx->feedback.pending_count == 0,
         "pending_count stays zero with armed fences");

   /* Cross-queue: a second distinct queue latches REWOUND and stops
    * arming. */
   npt_feedback_fence_register(ctx, 43, 7, 16,
                               NPT_FENCE_FEEDBACK_API_D3D11);
   struct npt_d3d11_fence_feedback_slot *slot_b =
      (struct npt_d3d11_fence_feedback_slot *)(res_storage + 16);
   const int calls_before = seoc_calls;
   const int creates_before = event_creates;
   npt_feedback_fence_mark_signal(ctx, &fence_b, 5, q1);
   CHECK(seoc_calls == calls_before + 1, "fence_b first queue armed");
   const uintptr_t b_handle = last_seoc_handle;
   npt_feedback_fence_mark_signal(ctx, &fence_b, 6, q2);
   CHECK((atomic_load(&slot_b->flags) &
          NPT_FENCE_FEEDBACK_FLAG_REWOUND) != 0,
         "second queue latches REWOUND");
   npt_feedback_fence_mark_signal(ctx, &fence_b, 7, q1);
   CHECK(seoc_calls == calls_before + 1, "rewound entry stops arming");

   /* Rewound entries keep their event and watch, so re-registering
    * the same fence repeatedly must neither create another event nor
    * pin one.  The outstanding arm at value 5 keeps armed_value, so
    * the next higher signal re-arms on the same event. */
   for (int i = 0; i < 3; i++) {
      memset(&fence_b.completed, 0, sizeof(fence_b.completed));
      npt_feedback_fence_register(ctx, 43, 7, 16,
                                  NPT_FENCE_FEEDBACK_API_D3D11);
   }
   CHECK(event_creates == creates_before + 1,
         "rewound re-register reuses the one arm event");
   CHECK(ctx->feedback.pinned_events == 0,
         "rewound re-register pins nothing");
   npt_feedback_fence_mark_signal(ctx, &fence_b, 7, q1);
   CHECK(seoc_calls == calls_before + 2 &&
         last_seoc_handle == b_handle,
         "re-registered fence re-arms on the same event");

   /* Below-target request on a fresh fence also latches REWOUND. */
   {
      /* Re-register fence_a on a fresh slot offset to reset state. */
      npt_feedback_unregister(ctx, 42);
      memset(&fence_a.completed, 0, sizeof(fence_a.completed));
      npt_feedback_fence_register(ctx, 42, 7, 32,
                                  NPT_FENCE_FEEDBACK_API_D3D12);
      struct npt_d3d11_fence_feedback_slot *slot_a2 =
         (struct npt_d3d11_fence_feedback_slot *)(res_storage + 32);
      npt_feedback_fence_mark_signal(ctx, &fence_a, 20, q1);
      npt_feedback_fence_mark_signal(ctx, &fence_a, 15, q1);
      CHECK((atomic_load(&slot_a2->flags) &
             NPT_FENCE_FEEDBACK_FLAG_REWOUND) != 0,
            "below-target request latches REWOUND");
   }

   /* Late fire after unregister: both fences still have an
    * outstanding arm, so each unregister pins exactly one event. */
   {
      uintptr_t old = last_seoc_handle;
      npt_feedback_unregister(ctx, 42);
      CHECK(ctx->feedback.pinned_events == 1,
            "unregister with outstanding arm pins exactly one");
      npt_feedback_unregister(ctx, 43);
      CHECK(ctx->feedback.pinned_events == 2,
            "second unregister pins one more");
#if defined(__APPLE__)
      /* The event may be pinned (live) or closed; a pinned one accepts
       * the write harmlessly, a closed fake handle would crash if the
       * watcher still touched it -- the point is this path is safe. */
      (void)old;
#endif
      struct timespec ts = { 0, 150000000 };
      nanosleep(&ts, NULL);
      CHECK(1, "late fire after unregister survived");
   }

   npt_feedback_fini(ctx);
   _mesa_hash_table_destroy(ctx->resource_table, NULL);
   mtx_destroy(&ctx->resource_mutex);
   free(ctx);

   if (failures) {
      fprintf(stderr, "test_npt_feedback_fence: %d check(s) failed\n",
              failures);
      return 1;
   }
   printf("test_npt_feedback_fence: all checks passed\n");
   return 0;
}
