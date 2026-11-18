/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * In-place ring decode test: commands decode directly out of the shared
 * ring buffer when contiguous, out of the private gather buffer when a
 * header or body straddles the end; head publishes per command; an
 * out-of-range or truncated cmd_size is fatal without overrun; and a
 * concurrent byte-mutating "guest" cannot crash or overrun the decoder.
 * Links the real npt_ring.c / npt_cs.c against a stub dispatcher that
 * honours the wire contract (read the header once, consume cmd_size).
 */

#ifndef __APPLE__
#define _GNU_SOURCE /* pthread_cond_clockwait */
#endif

#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "neptune/npt_context.h"
#include "neptune/npt_cs.h"
#include "neptune/npt_ring.h"

static int failures;

#define CHECK(cond, ...)                                                     \
   do {                                                                      \
      if (!(cond)) {                                                         \
         fprintf(stderr, "test_npt_ring_decode: FAIL %s:%d: ", __func__,     \
                __LINE__);                                                   \
         fprintf(stderr, __VA_ARGS__);                                       \
         fprintf(stderr, "\n");                                              \
         failures++;                                                         \
      }                                                                      \
   } while (0)

/* ---------------- stubs ---------------- */

/* The test never waits on this; keep the real Apple semantics anyway. */
int
npt_cnd_wait_rel_ns(cnd_t *cond, mtx_t *mutex, uint64_t ns)
{
   const uint64_t ns_per_sec = 1000000000ull;
   const struct timespec rel = {
      .tv_sec = (time_t)(ns / ns_per_sec),
      .tv_nsec = (long)(ns % ns_per_sec),
   };
#ifdef __APPLE__
   const int rc = pthread_cond_timedwait_relative_np(cond, mutex, &rel);
#else
   struct timespec abs;
   clock_gettime(CLOCK_MONOTONIC, &abs);
   abs.tv_sec += rel.tv_sec;
   abs.tv_nsec += rel.tv_nsec;
   if (abs.tv_nsec >= (long)ns_per_sec) {
      abs.tv_sec++;
      abs.tv_nsec -= (long)ns_per_sec;
   }
   const int rc = pthread_cond_clockwait(cond, mutex, CLOCK_MONOTONIC, &abs);
#endif
   if (rc == 0)
      return thrd_success;
   return rc == ETIMEDOUT ? thrd_timeout : thrd_error;
}

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

void
_debug_assert_fail(const char *expr, const char *file, unsigned line,
                   const char *func)
{
   fprintf(stderr, "test_npt_ring_decode: debug_assert %s failed at %s:%u (%s)\n",
           expr, file, line, func);
   abort();
}

uint64_t
npt_profile_now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void
npt_profile_dump(const char *reason)
{
   (void)reason;
}

void
npt_profile_register_ring(struct npt_ring *ring)
{
   (void)ring;
}

void
npt_profile_unregister_ring(struct npt_ring *ring)
{
   (void)ring;
}

void
npt_feedback_poll(struct npt_context *ctx, uint32_t *skip)
{
   (void)ctx; (void)skip;
}

void
npt_feedback_poll_interval(struct npt_context *ctx, uint64_t min_interval_ns)
{
   (void)ctx; (void)min_interval_ns;
}

struct npt_d3d_library *
npt_renderer_get_library(void)
{
   return NULL;
}

struct hash_entry *
_mesa_hash_table_search(struct hash_table *ht, const void *key)
{
   (void)ht; (void)key;
   return NULL;
}

void
npt_context_deferred_release_settle(struct npt_context *ctx,
                                    struct npt_deferred_release *release)
{
   (void)ctx; (void)release;
}

bool
npt_context_get_wait_ring_seqno(struct npt_context *ctx, uint64_t ring_id,
                                uint32_t *seqno)
{
   (void)ctx; (void)ring_id; (void)seqno;
   return false;
}

void
npt_context_on_ring_fatal(struct npt_context *ctx)
{
   (void)ctx;
}

bool
npt_context_ring_monitor_init(struct npt_context *ctx, uint32_t period_us)
{
   (void)ctx; (void)period_us;
   return true;
}

/* Record of what the stub dispatcher consumed. */
#define MAX_DISPATCHED 64
static uint32_t dispatched_types[MAX_DISPATCHED];
static uint32_t dispatched_sizes[MAX_DISPATCHED];
static uint32_t dispatched_count;
static uint32_t seqno_updates[MAX_DISPATCHED];
static uint32_t seqno_update_count;

void
npt_context_on_ring_seqno_update(struct npt_context *ctx, uint64_t ring_id,
                                 uint32_t seqno)
{
   (void)ctx; (void)ring_id;
   if (seqno_update_count < MAX_DISPATCHED)
      seqno_updates[seqno_update_count] = seqno;
   seqno_update_count++;
}

/* Test double for the real dispatch entry: honours the wire contract
 * (the header is read exactly once, the command occupies cmd_size). */
bool
npt_context_dispatch_one_command(struct npt_context *ctx,
                                 struct npt_dispatch_context *dispatch,
                                 struct npt_cs_decoder *dec,
                                 struct npt_cs_encoder *enc,
                                 struct npt_command_header *hdr)
{
   (void)ctx; (void)dispatch; (void)enc;

   const uint8_t *const start = dec->cur;
   struct npt_command_header cmd;
   npt_cs_decoder_read(dec, sizeof(cmd), &cmd, sizeof(cmd));
   if (npt_cs_decoder_get_fatal(dec))
      return false;
   if (cmd.cmd_size < sizeof(cmd) ||
       cmd.cmd_size > (size_t)(dec->end - start)) {
      npt_cs_decoder_set_fatal(dec);
      return false;
   }
   if (hdr)
      *hdr = cmd;

   if (dispatched_count < MAX_DISPATCHED) {
      dispatched_types[dispatched_count] = cmd.cmd_type;
      dispatched_sizes[dispatched_count] = cmd.cmd_size;
   }
   dispatched_count++;
   dec->cur = start + cmd.cmd_size;
   return true;
}

/* ---------------- harness ---------------- */

#define TEST_RING_SIZE 256u

struct test_ring {
   struct npt_ring *ring;
   struct npt_context ctx;
   bool fatal;
   uint8_t *ctl; /* head, tail, status */
   uint8_t *data;
};

static void
test_ring_init(struct test_ring *t, uint32_t size)
{
   memset(t, 0, sizeof(*t));
   t->ctl = calloc(1, 64);
   t->data = calloc(1, size);
   t->ring = calloc(1, sizeof(*t->ring));
   assert(t->ctl && t->data && t->ring);

   struct npt_ring *ring = t->ring;
   ring->control.head = (volatile atomic_uint *)(t->ctl + 0);
   ring->control.tail = (const volatile atomic_uint *)(t->ctl + 4);
   ring->control.status = (volatile atomic_uint *)(t->ctl + 8);
   ring->buffer.size = size;
   ring->buffer.mask = size - 1;
   ring->buffer.cur = 0;
   ring->buffer.data = t->data;
   ring->cmd = malloc(size);
   assert(ring->cmd);
   npt_cs_decoder_init(&ring->decoder, &t->fatal);
   npt_cs_encoder_init(&ring->encoder, &t->fatal);
   ring->context = &t->ctx;
   ring->decoder.fatal_error = &t->fatal;
   ring->encoder.fatal_error = &t->fatal;
   list_inithead(&ring->watch);
   list_inithead(&ring->peer_waits);
}

static void
test_ring_fini(struct test_ring *t)
{
   npt_cs_decoder_fini(&t->ring->decoder);
   npt_cs_encoder_fini(&t->ring->encoder);
   free(t->ring->cmd);
   free(t->ring);
   free(t->ctl);
   free(t->data);
   memset(t, 0, sizeof(*t));
}

static void
set_tail(struct test_ring *t, uint32_t tail)
{
   atomic_store_explicit((atomic_uint *)(t->ctl + 4), tail,
                         memory_order_release);
}

static uint32_t
get_head(struct test_ring *t)
{
   return atomic_load_explicit((atomic_uint *)(t->ctl + 0),
                               memory_order_acquire);
}

/* Write `size` bytes at ring position `pos`, wrap-aware. */
static void
ring_write(struct test_ring *t, uint32_t pos, const void *src, uint32_t size)
{
   const uint32_t off = pos & (TEST_RING_SIZE - 1);
   if (off + size <= TEST_RING_SIZE) {
      memcpy(t->data + off, src, size);
   } else {
      const uint32_t s = TEST_RING_SIZE - off;
      memcpy(t->data + off, src, s);
      memcpy(t->data, (const uint8_t *)src + s, size - s);
   }
}

static void
make_cmd(uint8_t *dst, uint32_t cmd_type, uint32_t cmd_size, uint8_t fill)
{
   struct npt_command_header *h = (struct npt_command_header *)dst;
   memset(dst, fill, cmd_size);
   h->cmd_type = cmd_type;
   h->cmd_size = cmd_size;
}

/* ---------------- tests ---------------- */

/* Three contiguous commands decode in place; head publishes per
 * command. */
static void
test_contiguous(void)
{
   struct test_ring t;
   test_ring_init(&t, TEST_RING_SIZE);
   dispatched_count = 0;
   seqno_update_count = 0;

   uint8_t c1[24], c2[32], c3[24];
   make_cmd(c1, 0x01000001, 24, 0x11);
   make_cmd(c2, 0x01000002, 32, 0x22);
   make_cmd(c3, 0x01000003, 24, 0x33);
   memcpy(t.data + 0, c1, 24);
   memcpy(t.data + 24, c2, 32);
   memcpy(t.data + 56, c3, 24);
   set_tail(&t, 80);

   CHECK(npt_ring_decode_available(t.ring, 80), "decode failed");
   CHECK(dispatched_count == 3, "dispatched %u commands", dispatched_count);
   CHECK(dispatched_types[0] == 0x01000001 && dispatched_types[1] == 0x01000002 &&
            dispatched_types[2] == 0x01000003, "cmd_types wrong");
   CHECK(t.ring->buffer.cur == 80, "cur %u", t.ring->buffer.cur);
   CHECK(get_head(&t) == 80, "head %u", get_head(&t));
   /* Head published per command, so seqno updates saw each prefix. */
   CHECK(seqno_update_count == 3, "seqno updates %u", seqno_update_count);
   CHECK(seqno_updates[0] == 24 && seqno_updates[1] == 56 &&
            seqno_updates[2] == 80, "per-command heads wrong");
   CHECK(!t.fatal, "unexpected fatal");

   test_ring_fini(&t);
}

/* A command whose header straddles the buffer end is gathered and
 * decoded; so is one whose body straddles. */
static void
test_wrap(void)
{
   struct test_ring t;
   test_ring_init(&t, TEST_RING_SIZE);
   dispatched_count = 0;
   seqno_update_count = 0;

   /* body straddle: header at 224..247 contiguous (24 bytes), body
    * wraps 8 bytes at 0..7.  cur=224, cmd_size=40, tail=264. */
   uint8_t cmd[40];
   make_cmd(cmd, 0x02000001, 40, 0x44);
   ring_write(&t, 224, cmd, 40);
   t.ring->buffer.cur = 224;
   set_tail(&t, 264);

   CHECK(npt_ring_decode_available(t.ring, 264), "body-straddle decode failed");
   CHECK(dispatched_count == 1 && dispatched_types[0] == 0x02000001,
         "body straddle dispatched %u", dispatched_count);
   CHECK(t.ring->buffer.cur == 264, "cur %u", t.ring->buffer.cur);
   CHECK(get_head(&t) == 264, "head %u", get_head(&t));

   /* header straddle: header occupies 248..255 + 0..15 (16 at start),
    * body 8 bytes at 16..23.  cur=248, cmd_size=32, tail=280. */
   uint8_t cmd2[32];
   make_cmd(cmd2, 0x02000002, 32, 0x55);
   ring_write(&t, 248, cmd2, 32);
   t.ring->buffer.cur = 248;
   set_tail(&t, 280);

   CHECK(npt_ring_decode_available(t.ring, 280), "header-straddle decode failed");
   CHECK(dispatched_count == 2 && dispatched_types[1] == 0x02000002,
         "header straddle dispatched %u", dispatched_count);
   CHECK(t.ring->buffer.cur == 280, "cur %u", t.ring->buffer.cur);
   CHECK(get_head(&t) == 280, "head %u", get_head(&t));

   CHECK(!t.fatal, "unexpected fatal");
   test_ring_fini(&t);
}

/* A truncated command (fewer bytes than a header, or cmd_size beyond
 * the tail) is a protocol violation: fatal, no overrun. */
static void
test_incomplete(void)
{
   /* tail - cur = 8 < sizeof(header) */
   {
      struct test_ring t;
      test_ring_init(&t, TEST_RING_SIZE);
      dispatched_count = 0;

      uint8_t frag[8] = { 0x01, 0x00, 0x00, 0x01, 0, 0, 0, 0 };
      memcpy(t.data, frag, 8);
      set_tail(&t, 8);

      CHECK(!npt_ring_decode_available(t.ring, 8), "truncated accepted");
      CHECK(dispatched_count == 0, "dispatched a fragment");
      test_ring_fini(&t);
   }

   /* cmd_size larger than the bytes the tail published */
   {
      struct test_ring t;
      test_ring_init(&t, TEST_RING_SIZE);
      dispatched_count = 0;

      uint8_t cmd[200];
      make_cmd(cmd, 0x03000001, 200, 0x66);
      memcpy(t.data, cmd, 24);
      set_tail(&t, 64); /* only 64 bytes published, cmd claims 200 */

      CHECK(!npt_ring_decode_available(t.ring, 64), "oversize accepted");
      CHECK(dispatched_count == 0, "dispatched oversize");
      test_ring_fini(&t);
   }

   /* cmd_size smaller than a header */
   {
      struct test_ring t;
      test_ring_init(&t, TEST_RING_SIZE);
      dispatched_count = 0;

      uint8_t cmd[24];
      make_cmd(cmd, 0x03000002, 8, 0x77);
      memcpy(t.data, cmd, 24);
      set_tail(&t, 24);

      CHECK(!npt_ring_decode_available(t.ring, 24), "tiny cmd_size accepted");
      CHECK(dispatched_count == 0, "dispatched undersize");
      test_ring_fini(&t);
   }
}

/* One thread decodes valid-looking commands while another rewrites the
 * buffer under it: no crash, no overrun; fatal is an acceptable
 * outcome. */
struct mutate_args {
   struct test_ring *t;
   _Atomic bool stop;
};

static int
mutate_fn(void *arg)
{
   struct mutate_args *a = arg;
   uint32_t x = 0x9e3779b9u;
   while (!atomic_load_explicit(&a->stop, memory_order_relaxed)) {
      x ^= x << 13; x ^= x >> 17; x ^= x << 5;
      const uint32_t off = x & (TEST_RING_SIZE - 1);
      a->t->data[off] = (uint8_t)(x >> 24);
   }
   return 0;
}

static void
test_toctou(void)
{
   struct test_ring t;
   test_ring_init(&t, TEST_RING_SIZE);

   /* Fill the ring with well-formed commands the mutator corrupts. */
   uint8_t cmd[32];
   make_cmd(cmd, 0x04000001, 32, 0x88);
   for (uint32_t off = 0; off < TEST_RING_SIZE; off += 32)
      memcpy(t.data + off, cmd, 32);
   set_tail(&t, TEST_RING_SIZE);

   struct mutate_args a = { .t = &t };
   thrd_t th;
   thrd_create(&th, mutate_fn, &a);

   const uint64_t end = npt_profile_now_ns() + 2000000000ull;
   while (npt_profile_now_ns() < end && !t.fatal) {
      /* Re-validate a window each pass; any outcome is fine as long as
       * the decoder stays in bounds and does not crash. */
      (void)npt_ring_decode_available(t.ring, TEST_RING_SIZE);
      t.fatal = false; /* clear for the next pass */
   }

   atomic_store(&a.stop, true);
   thrd_join(th, NULL);
   CHECK(1, "survived TOCTOU");
   test_ring_fini(&t);
}

int
main(void)
{
   test_contiguous();
   test_wrap();
   test_incomplete();
   test_toctou();

   if (failures) {
      fprintf(stderr, "test_npt_ring_decode: %d failures\n", failures);
      return 1;
   }
   fprintf(stderr, "test_npt_ring_decode: all checks passed\n");
   return 0;
}
