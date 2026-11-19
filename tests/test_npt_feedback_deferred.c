/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Deferred-context query Ends: an End decoded on a deferred context
 * bumps the version but is not on the immediate timeline until its
 * command list is executed, and D3DMetal would report the query's
 * previous result if polled earlier.  FinishCommandList moves the
 * deferred context's records under the command list and
 * ExecuteCommandList re-arms the entry.  Links the real npt_feedback.c
 * and npt_event_fd.c against stubs for the backend event library, COM
 * and the renderer.
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
         fprintf(stderr, "test_npt_feedback_deferred: FAIL %s\n", what); \
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
#if defined(__APPLE__)
struct fake_event {
   int rfd;
   int wfd;
};

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
#endif /* __APPLE__ */

static struct npt_d3d_library fake_lib;

struct npt_d3d_library *
npt_renderer_get_library(void)
{
   return &fake_lib;
}

/* ---------------- fake D3D11 COM ---------------- */

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

/* GetData on the immediate context: counts calls and always answers
 * S_OK with a BOOL TRUE result. */
static int getdata_calls;

static HRESULT
fake_get_data(void *self, void *async, void *data, UINT size, UINT flags)
{
   (void)self; (void)async; (void)flags;
   getdata_calls++;
   if (data && size >= sizeof(BOOL)) {
      BOOL v = 1;
      memcpy(data, &v, sizeof(v));
   }
   return 0; /* S_OK */
}

static UINT
fake_get_type_imm(void *self)
{
   (void)self;
   return 0; /* D3D11_DEVICE_CONTEXT_IMMEDIATE */
}

static UINT
fake_get_type_def(void *self)
{
   (void)self;
   return 1; /* D3D11_DEVICE_CONTEXT_DEFERRED */
}

struct fake_dev {
   void **vtbl;
   void *imm;
};

static struct fake_dev fake_dev_obj;

static void
fake_get_device(void *self, void **dev)
{
   (void)self;
   *dev = &fake_dev_obj;
}

static void
fake_get_imm(void *self, void **ctx)
{
   struct fake_dev *d = self;
   *ctx = d->imm;
}

/* Union pun, as NPT_COM_VTBL_FUNC uses in the other direction. */
#define VT(fn) (((union { void *p; __typeof__(&(fn)) f; }){ .f = (fn) }).p)

static void *imm_vtbl[128];
static void *def_vtbl[128];
static void *dev_vtbl[64];

struct fake_ctx {
   void **vtbl;
};

static struct fake_ctx imm_obj, def_a, def_b;
static int fake_query;

static void
fake_vtbls_init(void)
{
   imm_vtbl[0] = VT(fake_qi);
   imm_vtbl[1] = VT(fake_addref);
   imm_vtbl[2] = VT(fake_release);
   imm_vtbl[NPT_VTBL_ID3D11DeviceContext_GetType] = VT(fake_get_type_imm);
   imm_vtbl[NPT_VTBL_ID3D11DeviceContext_GetData] = VT(fake_get_data);

   def_vtbl[0] = VT(fake_qi);
   def_vtbl[1] = VT(fake_addref);
   def_vtbl[2] = VT(fake_release);
   def_vtbl[NPT_VTBL_ID3D11DeviceChild_GetDevice] = VT(fake_get_device);
   def_vtbl[NPT_VTBL_ID3D11DeviceContext_GetType] = VT(fake_get_type_def);

   dev_vtbl[0] = VT(fake_qi);
   dev_vtbl[1] = VT(fake_addref);
   dev_vtbl[2] = VT(fake_release);
   dev_vtbl[NPT_VTBL_ID3D11Device_GetImmediateContext] = VT(fake_get_imm);

   imm_obj.vtbl = imm_vtbl;
   def_a.vtbl = def_vtbl;
   def_b.vtbl = def_vtbl;
   fake_dev_obj.vtbl = dev_vtbl;
   fake_dev_obj.imm = &imm_obj;
}

/* ---------------- object/resource stubs ---------------- */

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
      return &fake_query;
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

static struct npt_query_feedback_slot *
qslot(void)
{
   return (struct npt_query_feedback_slot *)res_storage;
}

static uint64_t
qslot_state(void)
{
   return atomic_load_explicit(&qslot()->state, memory_order_acquire);
}

static void
poll_now(struct npt_context *ctx)
{
   npt_feedback_poll_interval(ctx, 0);
}

int
main(void)
{
   fake_vtbls_init();
#if defined(__APPLE__)
   fake_lib.pfn_event_create = fake_event_create;
   fake_lib.pfn_event_close = fake_event_close;
   fake_lib.pfn_event_dup_fd = fake_event_dup_fd;
#endif

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
   _mesa_hash_table_insert(ctx->resource_table, &fake_res.res_id,
                          &fake_res);

   npt_feedback_init(ctx);
   npt_feedback_query_register(ctx, 42, 7, 0, sizeof(BOOL));

   /* (1) Deferred End: version bumps, nothing pending or published
    * until the command list is executed. */
   npt_feedback_query_mark_end(ctx, &def_a, &fake_query);
   CHECK(ctx->feedback.pending_count == 0,
         "deferred End does not set pending");
   poll_now(ctx);
   CHECK(getdata_calls == 0, "deferred End does not reach GetData");
   CHECK((qslot_state() & NPT_QUERY_FEEDBACK_FLAG_READY) == 0,
         "deferred End publishes nothing");

   void *list1 = (void *)0x1111;
   npt_feedback_query_finish_list(ctx, &def_a, list1);
   poll_now(ctx);
   CHECK(getdata_calls == 0,
         "finished but unexecuted list still does not publish");

   npt_feedback_query_execute_list(ctx, list1);
   poll_now(ctx);
   CHECK(getdata_calls == 1, "executed list polls GetData once");
   CHECK((qslot_state() >> 32) == 1 &&
         (qslot_state() & NPT_QUERY_FEEDBACK_FLAG_READY) != 0,
         "executed deferred End publishes version 1");

   /* (2) Immediate End publishes as before. */
   npt_feedback_query_mark_end(ctx, &imm_obj, &fake_query);
   CHECK(ctx->feedback.pending_count == 1,
         "immediate End sets pending at once");
   poll_now(ctx);
   CHECK(getdata_calls == 2 && (qslot_state() >> 32) == 2,
         "immediate End publishes version 2");

   /* (3) Executing a command list with no records is a no-op. */
   npt_feedback_query_execute_list(ctx, (void *)0x2222);
   poll_now(ctx);
   CHECK(getdata_calls == 2 && (qslot_state() >> 32) == 2,
         "recordless list changes nothing");

   /* (4) Two Ends on one query in two command lists: only the newest
    * submitted version may publish. */
   npt_feedback_query_mark_end(ctx, &def_a, &fake_query);   /* v3 */
   npt_feedback_query_finish_list(ctx, &def_a, (void *)0x3333);
   npt_feedback_query_mark_end(ctx, &def_a, &fake_query);   /* v4 */
   npt_feedback_query_finish_list(ctx, &def_a, (void *)0x4444);

   npt_feedback_query_execute_list(ctx, (void *)0x3333);
   poll_now(ctx);
   CHECK(getdata_calls == 2 && (qslot_state() >> 32) == 2,
         "stale list does not publish over a newer unexecuted End");

   npt_feedback_query_execute_list(ctx, (void *)0x4444);
   poll_now(ctx);
   CHECK(getdata_calls == 3 && (qslot_state() >> 32) == 4,
         "newest list publishes version 4");

   /* A deferred context released mid-stream keeps its records until
    * teardown; fini must free them (also covers a dropped-finish). */
   npt_feedback_query_mark_end(ctx, &def_b, &fake_query);   /* v5 */
   npt_feedback_query_mark_end(ctx, &def_a, &fake_query);   /* v6 */
   npt_feedback_query_finish_list(ctx, &def_a, NULL);

   npt_feedback_fini(ctx);
   _mesa_hash_table_destroy(ctx->resource_table, NULL);
   mtx_destroy(&ctx->resource_mutex);
   free(ctx);

   if (failures) {
      fprintf(stderr, "test_npt_feedback_deferred: %d check(s) failed\n",
              failures);
      return 1;
   }
   printf("test_npt_feedback_deferred: all checks passed\n");
   return 0;
}
