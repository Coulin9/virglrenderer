/*
 * Copyright 2026
 * SPDX-License-Identifier: MIT
 */

/* Unit and stress tests for the shared submit ring (server/render_ring.h).
 *
 * Self-contained on purpose: the ring helpers are header-only, so this
 * test needs neither check nor libvirglrenderer and can also be built for
 * x86_64 to exercise the worker side under Rosetta.
 */

#include "server/render_ring.h"

#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>

static int failures;

#define CHECK(cond, ...)                                                       \
   do {                                                                        \
      if (!(cond)) {                                                           \
         fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);                  \
         fprintf(stderr, __VA_ARGS__);                                         \
         fprintf(stderr, "\n");                                                \
         failures++;                                                           \
      }                                                                        \
   } while (0)

static int
make_pipe(int fds[2])
{
   if (pipe(fds))
      return -1;
   for (int i = 0; i < 2; i++) {
      const int fdflags = fcntl(fds[i], F_GETFD);
      const int stflags = fcntl(fds[i], F_GETFL);
      if (fdflags < 0 || fcntl(fds[i], F_SETFD, fdflags | FD_CLOEXEC) ||
          stflags < 0 || fcntl(fds[i], F_SETFL, stflags | O_NONBLOCK))
         return -1;
   }
#ifdef F_SETNOSIGPIPE
   fcntl(fds[1], F_SETNOSIGPIPE, 1);
#endif
   return 0;
}

struct test_region {
   struct render_ring_control *ctl;
   uint8_t *base;
   struct render_ring_layout layout;
};

static void
test_region_init(struct test_region *r, bool has_oob)
{
   render_ring_layout_compute(&r->layout, has_oob, false);
   r->base = mmap(NULL, r->layout.total_size, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_ANON, -1, 0);
   r->ctl = (void *)r->base;
   render_ring_control_init(r->ctl);
}

static void
test_region_fini(struct test_region *r)
{
   munmap(r->base, r->layout.total_size);
}

static struct render_ring_buf
test_main_buf(struct test_region *r)
{
   return render_ring_main_buf(r->ctl, r->base, r->layout.main_offset,
                               r->layout.main_size);
}

/* deterministic PRNG (splitmix64-style) */
struct rng {
   uint64_t s;
};

static uint64_t
rng_next(struct rng *r)
{
   uint64_t x = r->s;
   x ^= x >> 12;
   x ^= x << 25;
   x ^= x >> 27;
   r->s = x;
   return x * 0x2545F4914F6CDD1Dull;
}

static uint32_t
rng_range(struct rng *r, uint32_t n)
{
   return (uint32_t)(rng_next(r) % n);
}

/* ------------------------------------------------------------------ */
/* encode/decode unit cases                                            */

/* a small standalone ring for encode/decode cases: entries stay under the
 * 256 KiB SUBMIT_CMD bound while still exercising wrap/pad edges */
#define SMALL_RING_SIZE 4096

struct small_ring {
   atomic_uint head;
   atomic_uint tail;
   uint8_t data[SMALL_RING_SIZE];
   struct render_ring_buf r;
};

static void
small_ring_init(struct small_ring *s)
{
   atomic_init(&s->head, 0);
   atomic_init(&s->tail, 0);
   memset(s->data, 0, sizeof(s->data));
   s->r = (struct render_ring_buf){
      .head = &s->head,
      .tail = &s->tail,
      .data = s->data,
      .size = SMALL_RING_SIZE,
   };
}

static void
test_encode_decode(void)
{
   struct small_ring small;
   small_ring_init(&small);
   struct render_ring_buf ring = small.r;
   uint8_t scratch[SMALL_RING_SIZE] = { 0 };
   uint32_t head = 0;

   /* basic roundtrip */
   const uint8_t payload[13] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
   CHECK(render_ring_push(&ring, RENDER_RING_OP_SUBMIT_CMD, payload,
                          sizeof(payload)),
         "basic push failed");
   uint32_t op, size;
   const uint8_t *out;
   CHECK(render_ring_read(&ring, &head, &op, &out, &size) == RENDER_RING_READ_OK,
         "basic read failed");
   CHECK(op == RENDER_RING_OP_SUBMIT_CMD && size == sizeof(payload) &&
            !memcmp(out, payload, size),
         "basic entry mismatch");
   render_ring_commit(&ring, head);
   CHECK(render_ring_read(&ring, &head, &op, &out, &size) ==
            RENDER_RING_READ_EMPTY,
         "ring not empty after drain");

   /* PAD wrap: leave only 64 contiguous bytes at the end, then push an
    * entry that needs 72 -> a PAD record covers the tail and the entry
    * continues at offset 0 */
   const uint32_t big_size = SMALL_RING_SIZE - 64 - 8; /* ends at size-64 */
   CHECK(render_ring_push(&ring, RENDER_RING_OP_SUBMIT_CMD, scratch, big_size),
         "big push failed");
   CHECK(render_ring_read(&ring, &head, &op, &out, &size) ==
            RENDER_RING_READ_OK &&
            size == big_size,
         "big read failed");
   render_ring_commit(&ring, head);

   const uint8_t wrap_payload[64] = { 0xaa };
   CHECK(render_ring_push(&ring, RENDER_RING_OP_SUBMIT_CMD, wrap_payload,
                          sizeof(wrap_payload)),
         "wrap push failed");
   CHECK(render_ring_read(&ring, &head, &op, &out, &size) ==
            RENDER_RING_READ_OK &&
            size == sizeof(wrap_payload) &&
            !memcmp(out, wrap_payload, size),
         "wrapped entry mismatch (op %u size %u)", op, size);
   render_ring_commit(&ring, head);

   /* entry exactly filling the end: position tail at size - (8+16) so an
    * entry of size 16 lands flush against the ring end without a pad */
   {
      small_ring_init(&small);
      ring = small.r;
      head = 0;
      const uint32_t tail_target = SMALL_RING_SIZE - (8 + 16);
      CHECK(render_ring_push(&ring, RENDER_RING_OP_SUBMIT_CMD, scratch,
                             tail_target - 8),
            "exact-fill setup push failed");
      CHECK(render_ring_read(&ring, &head, &op, &out, &size) ==
               RENDER_RING_READ_OK,
            "exact-fill setup read failed");
      render_ring_commit(&ring, head);
      const uint8_t tail_payload[16] = { 0x55 };
      CHECK(render_ring_push(&ring, RENDER_RING_OP_SUBMIT_CMD, tail_payload,
                             sizeof(tail_payload)),
            "exact-fill push failed");
      CHECK(render_ring_read(&ring, &head, &op, &out, &size) ==
               RENDER_RING_READ_OK &&
               size == 16 && !memcmp(out, tail_payload, 16),
            "exact-fill read failed");
      render_ring_commit(&ring, head);
   }

   /* max inline size: a 256 KiB SUBMIT_CMD fits the 1 MiB shared ring */
   {
      struct test_region region;
      test_region_init(&region, false);
      struct render_ring_buf big_ring = test_main_buf(&region);
      uint32_t bhead = 0;
      uint8_t *big = malloc(RENDER_RING_MAX_INLINE_CMD);
      memset(big, 0x5a, RENDER_RING_MAX_INLINE_CMD);
      CHECK(render_ring_push(&big_ring, RENDER_RING_OP_SUBMIT_CMD, big,
                             RENDER_RING_MAX_INLINE_CMD),
            "max-inline push failed");
      CHECK(render_ring_read(&big_ring, &bhead, &op, &out, &size) ==
               RENDER_RING_READ_OK &&
               size == RENDER_RING_MAX_INLINE_CMD && out[0] == 0x5a &&
               out[RENDER_RING_MAX_INLINE_CMD - 1] == 0x5a,
            "max-inline read failed");
      render_ring_commit(&big_ring, bhead);
      free(big);
      test_region_fini(&region);
   }

   /* malformed entries */
   {
      /* unknown op */
      small_ring_init(&small);
      ring = small.r;
      head = 0;
      struct render_ring_entry_header bad = { .op = 0xdead, .size = 8 };
      memcpy(ring.data, &bad, sizeof(bad));
      atomic_store_explicit(ring.tail, (uint32_t)sizeof(bad),
                            memory_order_release);
      CHECK(render_ring_read(&ring, &head, &op, &out, &size) ==
               RENDER_RING_READ_CORRUPT,
            "unknown op not rejected");

      /* size beyond the SUBMIT_CMD bound (oversize must take the socket
       * path instead) */
      atomic_store_explicit(&small.head, 0, memory_order_relaxed);
      atomic_store_explicit(&small.tail, 0, memory_order_relaxed);
      head = 0;
      struct render_ring_entry_header big = {
         .op = RENDER_RING_OP_SUBMIT_CMD,
         .size = RENDER_RING_MAX_INLINE_CMD + 8,
      };
      memcpy(ring.data, &big, sizeof(big));
      atomic_store_explicit(ring.tail, (uint32_t)sizeof(big),
                            memory_order_release);
      CHECK(render_ring_read(&ring, &head, &op, &out, &size) ==
               RENDER_RING_READ_CORRUPT,
            "oversize SUBMIT_CMD not rejected");

      /* head lagging tail by more than the ring capacity */
      atomic_store_explicit(&small.head, 0, memory_order_relaxed);
      atomic_store_explicit(&small.tail, SMALL_RING_SIZE + 8,
                            memory_order_release);
      head = 0;
      CHECK(render_ring_read(&ring, &head, &op, &out, &size) ==
               RENDER_RING_READ_CORRUPT,
            "head>tail sanity not rejected");

      /* truncated entry: header promises payload past tail */
      atomic_store_explicit(&small.head, 0, memory_order_relaxed);
      head = 0;
      struct render_ring_entry_header trunc = {
         .op = RENDER_RING_OP_SUBMIT_CMD,
         .size = 64,
      };
      memcpy(ring.data, &trunc, sizeof(trunc));
      atomic_store_explicit(&small.tail, 16, memory_order_release);
      CHECK(render_ring_read(&ring, &head, &op, &out, &size) ==
               RENDER_RING_READ_CORRUPT,
            "torn entry not rejected");
   }
}

/* ------------------------------------------------------------------ */
/* two-process stress test                                             */

#define STRESS_ENTRIES 1000000
#define STRESS_MAX_PAYLOAD 4096
#define STRESS_COMPLETIONS 200000

static const uint64_t pause_ns_table[] = { 0, 1000, 10000, 100000, 1000000 };

static uint8_t
payload_byte(uint32_t entry_idx, uint32_t j)
{
   return (uint8_t)(entry_idx * 2654435761u + j * 40503u);
}

struct main_consumer {
   const struct render_ring_buf *r;
   uint32_t head;
};

static bool
main_ring_has_work(void *opaque)
{
   const struct main_consumer *c = opaque;
   return render_ring_has_entries(c->r, c->head);
}

struct compl_consumer {
   struct render_ring_control *ctl;
   uint32_t *last;
};

static bool
compl_has_work(void *opaque)
{
   const struct compl_consumer *c = opaque;
   return atomic_load_explicit(&c->ctl->timelines[0], memory_order_acquire) !=
          *c->last;
}

static void
stress_producer(struct test_region *region, int main_wake_w, int compl_w)
{
   struct render_ring_buf ring = test_main_buf(region);
   struct rng sizes = { .s = 0x9e3779b97f4a7c15ull };
   struct rng burst = { .s = 0xd1b54a32d192ed03ull };
   uint8_t *buf = malloc(STRESS_MAX_PAYLOAD + 8);

   const struct timespec ns20 = { .tv_sec = 0, .tv_nsec = 20000 };

   uint32_t i = 0;
   while (i < STRESS_ENTRIES) {
      const uint32_t burst_len = 1 + rng_range(&burst, 64);
      for (uint32_t b = 0; b < burst_len && i < STRESS_ENTRIES; b++, i++) {
         const uint32_t size = rng_range(&sizes, STRESS_MAX_PAYLOAD + 1);
         for (uint32_t j = 0; j < size; j++)
            buf[j] = payload_byte(i, j);
         while (!render_ring_push(&ring, RENDER_RING_OP_SUBMIT_CMD, buf, size))
            nanosleep(&ns20, NULL);
         atomic_fetch_add(&region->ctl->stats_vmm.v.main_entries, 1);
         render_ring_producer_wake(&region->ctl->main_state.value, main_wake_w,
                                   &region->ctl->stats_vmm.v.main_wakes);
      }
      const uint64_t pause = pause_ns_table[rng_range(&burst, 5)];
      if (pause) {
         const struct timespec ts = {
            .tv_sec = (time_t)(pause / 1000000000ull),
            .tv_nsec = (long)(pause % 1000000000ull),
         };
         nanosleep(&ts, NULL);
      }
   }

   /* completion direction: publish seqno advances */
   for (uint32_t s = 1; s <= STRESS_COMPLETIONS; s++) {
      atomic_store_explicit(&region->ctl->timelines[0], s,
                            memory_order_release);
      render_ring_producer_wake(&region->ctl->vmm_state.value, compl_w,
                                &region->ctl->stats_worker.v.fence_notifies);
   }

   free(buf);
   _exit(0);
}

static int
stress_consumer(struct test_region *region,
                int main_wake_r,
                int compl_r,
                int death_r,
                pid_t child)
{
   struct render_ring_buf ring = test_main_buf(region);
   struct rng sizes = { .s = 0x9e3779b97f4a7c15ull };
   struct main_consumer consumer = { .r = &ring, .head = 0 };

   uint32_t consumed = 0;
   bool child_reaped = false;
   bool child_dead = false;

   while (consumed < STRESS_ENTRIES || !child_dead) {
      uint32_t op, size;
      const uint8_t *payload;
      const enum render_ring_read_result res =
         render_ring_read(&ring, &consumer.head, &op, &payload, &size);
      if (res == RENDER_RING_READ_CORRUPT) {
         fprintf(stderr, "FAIL: corrupt entry at %u\n", consumer.head);
         return 1;
      }
      if (res == RENDER_RING_READ_OK) {
         const uint32_t expect = rng_range(&sizes, STRESS_MAX_PAYLOAD + 1);
         if (op != RENDER_RING_OP_SUBMIT_CMD || size != expect) {
            fprintf(stderr, "FAIL: entry %u op %u size %u expected %u\n",
                    consumed, op, size, expect);
            return 1;
         }
         for (uint32_t j = 0; j < size; j++) {
            if (payload[j] != payload_byte(consumed, j)) {
               fprintf(stderr, "FAIL: entry %u payload byte %u corrupted\n",
                       consumed, j);
               return 1;
            }
         }
         consumed++;
         render_ring_commit(&ring, consumer.head);
         continue;
      }

      /* EMPTY: park with the real wake helper.  A bounded poll catches
       * lost wakeups: if the ring has work while we timed out asleep,
       * nobody signalled. */
      const enum render_ring_wait_result wr = render_ring_consumer_wait(
         &region->ctl->main_state.value, main_wake_r, death_r,
         main_ring_has_work, &consumer, 0 /* spin_ns */, 100 /* ms */,
         &region->ctl->stats_worker.v.main_sleeps);
      if (wr == RENDER_RING_WAIT_ERROR)
         return 1;
      if (wr == RENDER_RING_WAIT_TIMEOUT) {
         if (render_ring_has_entries(&ring, consumer.head)) {
            fprintf(stderr, "FAIL: lost wakeup at %u entries\n", consumed);
            return 1;
         }
         if (!child_reaped && waitpid(child, NULL, WNOHANG) == child) {
            child_reaped = true;
            child_dead = true;
         }
      }
      if (wr == RENDER_RING_WAIT_FD1) {
         char b;
         if (read(death_r, &b, 1) <= 0)
            child_dead = true;
      }
   }

   /* completion direction: the child published seqno advances; consume
    * them with the sync-thread wake logic */
   uint32_t last = 0;
   struct compl_consumer compl_ctx = { .ctl = region->ctl, .last = &last };

   while (last < STRESS_COMPLETIONS) {
      last = atomic_load_explicit(&region->ctl->timelines[0],
                                  memory_order_acquire);
      if (last >= STRESS_COMPLETIONS)
         break;
      const enum render_ring_wait_result wr = render_ring_consumer_wait(
         &region->ctl->vmm_state.value, compl_r, -1, compl_has_work,
         &compl_ctx, 0 /* spin_ns */, 100 /* ms */,
         &region->ctl->stats_vmm.v.vmm_sleeps);
      if (wr == RENDER_RING_WAIT_ERROR)
         return 1;
      if (wr == RENDER_RING_WAIT_TIMEOUT &&
          atomic_load_explicit(&region->ctl->timelines[0],
                               memory_order_acquire) != last) {
         fprintf(stderr, "FAIL: lost completion wakeup at %u\n", last);
         return 1;
      }
   }

   if (last != STRESS_COMPLETIONS) {
      fprintf(stderr, "FAIL: completion count %u != %u\n", last,
              STRESS_COMPLETIONS);
      return 1;
   }

   return 0;
}

static int
test_two_process(void)
{
   struct test_region region;
   test_region_init(&region, false);

   int main_wake[2], compl[2], death[2];
   if (make_pipe(main_wake) || make_pipe(compl) || make_pipe(death))
      return 1;

   const pid_t child = fork();
   if (child < 0)
      return 1;
   if (child == 0) {
      close(main_wake[0]);
      close(compl[0]);
      close(death[0]);
      stress_producer(&region, main_wake[1], compl[1]);
   }

   close(main_wake[1]);
   close(compl[1]);
   close(death[1]);

   const int ret = stress_consumer(&region, main_wake[0], compl[0], death[0],
                                   child);

   int status = 0;
   waitpid(child, &status, 0);
   close(main_wake[0]);
   close(compl[0]);
   close(death[0]);
   test_region_fini(&region);

   CHECK(ret == 0, "stress consumer failed");
   CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "producer child failed (status %d)", status);
   return ret;
}

/* ------------------------------------------------------------------ */
/* death test                                                          */

static int
test_death(void)
{
   struct test_region region;
   test_region_init(&region, false);

   int main_wake[2], death[2];
   if (make_pipe(main_wake) || make_pipe(death))
      return 1;

   const pid_t child = fork();
   if (child < 0)
      return 1;
   if (child == 0) {
      close(death[0]);
      close(main_wake[0]);
      /* produce a few entries, then die mid-stream holding the death
       * pipe's write end */
      struct render_ring_buf ring = test_main_buf(&region);
      const uint8_t p[8] = { 0 };
      for (int i = 0; i < 16; i++)
         render_ring_push(&ring, RENDER_RING_OP_SUBMIT_CMD, p, sizeof(p));
      _exit(0);
   }

   close(death[1]);
   close(main_wake[1]);

   /* the death pipe must report EOF promptly */
   struct pollfd pf = { .fd = death[0], .events = POLLIN };
   const int pr = poll(&pf, 1, 2000);
   CHECK(pr == 1, "death pipe did not fire (poll=%d revents=%x)", pr,
         pf.revents);
   char b;
   const ssize_t rd = read(death[0], &b, 1);
   CHECK(rd <= 0, "death pipe not at EOF (read=%zd)", rd);

   /* producer-side contract: once the worker is dead, pushes must bail
    * with an error instead of hanging.  Mirror the production
    * worker_dead check on a now-full ring. */
   _Atomic bool worker_dead = true;
   struct render_ring_buf ring = test_main_buf(&region);
   uint8_t *scratch = calloc(1, 65536);
   while (render_ring_push(&ring, RENDER_RING_OP_SUBMIT_CMD, scratch, 65528)) {
   }
   while (render_ring_push(&ring, RENDER_RING_OP_SUBMIT_CMD, scratch, 8)) {
   }
   /* the ring is now so full that not even a header fits contiguously;
    * the next push can only succeed after a wrap+pad, which needs space
    * the dead consumer will never free */
   while (render_ring_push(&ring, RENDER_RING_OP_SUBMIT_CMD, scratch,
                           RENDER_RING_MAX_INLINE_CMD - 8)) {
   }

   bool pushed = true;
   const struct timespec backoff = { .tv_sec = 0, .tv_nsec = 20000 };
   for (;;) {
      if (render_ring_push(&ring, RENDER_RING_OP_SUBMIT_CMD, scratch, 8))
         break;
      if (atomic_load(&worker_dead)) {
         pushed = false;
         break;
      }
      nanosleep(&backoff, NULL);
   }
   CHECK(!pushed, "producer kept pushing on a full ring with a dead worker");
   free(scratch);

   int status;
   waitpid(child, &status, 0);
   close(death[0]);
   close(main_wake[0]);
   test_region_fini(&region);
   return 0;
}

static void
test_lat_layout(void)
{
   struct render_ring_layout plain, traced;
   render_ring_layout_compute(&plain, true, false);
   render_ring_layout_compute(&traced, true, true);

   /* without the flag the region is exactly the rings: no trailing pad */
   CHECK(plain.lat_offset == 0 && plain.lat_size == 0,
         "plain layout carries lat fields");
   CHECK(plain.total_size == plain.oob_offset + plain.oob_size,
         "plain layout grew a tail pad");

   /* with the flag the area sits on a 128B boundary after the oob ring */
   const uint64_t rings_end = (uint64_t)plain.oob_offset + plain.oob_size;
   const uint32_t expected_off =
      (uint32_t)((rings_end + RENDER_RING_CACHELINE - 1) &
                 ~(uint64_t)(RENDER_RING_CACHELINE - 1));
   CHECK(traced.lat_offset == expected_off &&
            traced.lat_offset >= rings_end,
         "lat offset %u not cacheline-aligned after rings (end %llu)",
         traced.lat_offset, (unsigned long long)rings_end);
   CHECK(traced.lat_size == sizeof(struct render_ring_lat_area),
         "lat size %u != %zu", traced.lat_size,
         sizeof(struct render_ring_lat_area));
   CHECK(traced.total_size == traced.lat_offset + traced.lat_size,
         "traced total_size mismatch");

   /* the traced layout must validate; the plain one must not pretend to
    * have an area, and a truncated region must fail */
   CHECK(render_ring_layout_valid(traced.total_size, traced.main_offset,
                                  traced.main_size, traced.oob_offset,
                                  traced.oob_size, traced.lat_offset,
                                  traced.lat_size),
         "traced layout rejected");
   CHECK(render_ring_layout_valid(plain.total_size, plain.main_offset,
                                  plain.main_size, plain.oob_offset,
                                  plain.oob_size, 0, 0),
         "plain layout rejected");
   CHECK(!render_ring_layout_valid(plain.total_size, plain.main_offset,
                                   plain.main_size, plain.oob_offset,
                                   plain.oob_size, expected_off,
                                   traced.lat_size),
         "lat area accepted without room");
   CHECK(!render_ring_layout_valid(traced.total_size - 1, traced.main_offset,
                                   traced.main_size, traced.oob_offset,
                                   traced.oob_size, traced.lat_offset,
                                   traced.lat_size),
         "undersized shmem accepted");
   CHECK(!render_ring_layout_valid(traced.total_size, traced.main_offset,
                                   traced.main_size, traced.oob_offset,
                                   traced.oob_size, traced.lat_offset,
                                   traced.lat_size - 8),
         "wrong lat size accepted");

   /* ring payloads and OOB slots keep their pre-trace formats; only the
    * latency area may carry fence_id */
   CHECK(sizeof(struct render_ring_fence) == 12,
         "render_ring_fence size %zu != 12", sizeof(struct render_ring_fence));
   CHECK(sizeof(struct render_ring_oob_slot) == 16,
         "render_ring_oob_slot size %zu != 16",
         sizeof(struct render_ring_oob_slot));
   CHECK(offsetof(struct render_ring_fence, flags) == 0 &&
            offsetof(struct render_ring_fence, ring_idx) == 4 &&
            offsetof(struct render_ring_fence, seqno) == 8,
         "render_ring_fence field offsets changed");

   /* seqmap: the proxy publishes (ring_idx, seqno) -> fence_id before
    * the push; the worker's acquire+seqno match resolves it */
   uint8_t *shm = calloc(1, traced.total_size);
   CHECK(shm != NULL, "shm alloc");
   if (shm) {
      struct render_ring_lat_area *lat =
         render_ring_lat_area(shm, traced.lat_offset);
      CHECK(lat != NULL, "lat area pointer");
      struct render_ring_lat_seqent *se =
         render_ring_lat_seqmap(lat, 3, 77);
      atomic_store_explicit(&se->seqno, 77, memory_order_relaxed);
      atomic_store_explicit(&se->fence_id, 0xdeadbeef, memory_order_release);
      const uint64_t fid =
         atomic_load_explicit(&se->fence_id, memory_order_acquire);
      CHECK(fid == 0xdeadbeef &&
               atomic_load_explicit(&se->seqno, memory_order_acquire) == 77,
            "seqmap roundtrip failed");
      /* a different seqno aliasing the same slot must not match */
      struct render_ring_lat_seqent *alias =
         render_ring_lat_seqmap(lat, 3, 77 + RENDER_RING_LAT_SEQMAP_STRIDE);
      CHECK(alias == se, "seqmap stride alias did not land on same slot");
      atomic_store_explicit(&alias->seqno, 77 + RENDER_RING_LAT_SEQMAP_STRIDE,
                            memory_order_relaxed);
      atomic_store_explicit(&alias->fence_id, 0x1234, memory_order_release);
      CHECK(atomic_load_explicit(&se->seqno, memory_order_acquire) != 77,
            "seqmap seqno check cannot reject a stale entry");
      free(shm);
   }
}

int
main(void)
{
   test_encode_decode();
   test_lat_layout();
   if (test_two_process())
      failures++;
   if (test_death())
      failures++;

   if (failures) {
      fprintf(stderr, "%d check(s) failed\n", failures);
      return 1;
   }
   printf("test_render_ring: all checks passed\n");
   return 0;
}
