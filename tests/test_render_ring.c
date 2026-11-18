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
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <spawn.h>

/* Not declared by the SDK <spawn.h>; exported by libSystem. */
extern int posix_spawnattr_set_registered_ports_np(posix_spawnattr_t *,
                                                   const mach_port_t *,
                                                   uint32_t);
#else
#include <sys/eventfd.h>
#endif

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
/* wake primitive helpers                                              */

static render_wake_t
wake_create(void)
{
#if defined(__APPLE__)
   semaphore_t sem;
   if (semaphore_create(mach_task_self(), &sem, SYNC_POLICY_FIFO, 0))
      return RENDER_WAKE_NULL;
   return sem;
#else
   const int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
   return fd < 0 ? RENDER_WAKE_NULL : fd;
#endif
}

static void
wake_destroy(render_wake_t w)
{
   if (w == RENDER_WAKE_NULL)
      return;
#if defined(__APPLE__)
   semaphore_destroy(mach_task_self(), w);
#else
   close(w);
#endif
}

/* ------------------------------------------------------------------ */
/* single-process conditional wake                                     */

#define SP_ITEMS 20000
#define SP_MAX_PAYLOAD 512

static uint8_t
payload_byte(uint32_t entry_idx, uint32_t j)
{
   return (uint8_t)(entry_idx * 2654435761u + j * 40503u);
}

struct consumer_args {
   struct test_region *region;
   struct render_ring_buf ring;
   render_wake_t wake;
   uint32_t consumed_head;
   uint32_t consumed;
   atomic_bool stop;
   int error;
};

static bool
main_ring_has_work(void *opaque)
{
   const struct consumer_args *c = opaque;
   return render_ring_has_entries(&c->ring, c->consumed_head);
}

static void *
consumer_thread(void *arg)
{
   struct consumer_args *c = arg;
   struct rng sizes = { .s = 0x9e3779b97f4a7c15ull };

   while (c->consumed < SP_ITEMS && !atomic_load(&c->stop)) {
      uint32_t op, size;
      const uint8_t *payload;
      const enum render_ring_read_result res =
         render_ring_read(&c->ring, &c->consumed_head, &op, &payload, &size);
      if (res == RENDER_RING_READ_CORRUPT) {
         fprintf(stderr, "FAIL: corrupt entry\n");
         c->error = 1;
         return NULL;
      }
      if (res == RENDER_RING_READ_OK) {
         const uint32_t expect = rng_range(&sizes, SP_MAX_PAYLOAD + 1);
         if (op != RENDER_RING_OP_SUBMIT_CMD || size != expect) {
            fprintf(stderr, "FAIL: entry %u size %u expected %u\n",
                    c->consumed, size, expect);
            c->error = 1;
            return NULL;
         }
         for (uint32_t j = 0; j < size; j++) {
            if (payload[j] != payload_byte(c->consumed, j)) {
               fprintf(stderr, "FAIL: entry %u byte %u corrupted\n",
                       c->consumed, j);
               c->error = 1;
               return NULL;
            }
         }
         c->consumed++;
         render_ring_commit(&c->ring, c->consumed_head);
         continue;
      }

      const enum render_ring_wait_result wr = render_ring_consumer_wait(
         &c->region->ctl->main_state.value, c->wake, main_ring_has_work, c,
         0 /* spin_ns */, -1 /* infinite */, NULL);
      if (wr == RENDER_RING_WAIT_ERROR) {
         c->error = 1;
         return NULL;
      }
      /* WORK and WOKEN both fall back to draining */
   }
   return NULL;
}

/* consumer thread parks on the wake primitive while the producer pushes
 * with pauses so it really sleeps; checks the full state machine */
static void
test_conditional_wake(void)
{
   struct test_region region;
   test_region_init(&region, false);

   struct consumer_args c = {
      .region = &region,
      .ring = test_main_buf(&region),
      .wake = wake_create(),
      .consumed_head = 0,
      .consumed = 0,
   };
   atomic_init(&c.stop, false);
   CHECK(c.wake != RENDER_WAKE_NULL, "wake_create failed");

   pthread_t tid;
   CHECK(pthread_create(&tid, NULL, consumer_thread, &c) == 0,
         "pthread_create failed");

   struct rng sizes = { .s = 0x9e3779b97f4a7c15ull };
   struct rng pause_rng = { .s = 0xd1b54a32d192ed03ull };
   uint8_t *buf = malloc(SP_MAX_PAYLOAD + 8);
   const struct timespec pause = { .tv_sec = 0, .tv_nsec = 500000 };
   const struct timespec backoff = { .tv_sec = 0, .tv_nsec = 20000 };

   uint64_t entries = 0;
   for (uint32_t i = 0; i < SP_ITEMS; i++) {
      const uint32_t size = rng_range(&sizes, SP_MAX_PAYLOAD + 1);
      for (uint32_t j = 0; j < size; j++)
         buf[j] = payload_byte(i, j);
      while (!render_ring_push(&c.ring, RENDER_RING_OP_SUBMIT_CMD, buf, size))
         nanosleep(&backoff, NULL);
      entries++;
      render_ring_producer_wake(&region.ctl->main_state.value, c.wake, NULL);
      /* pace the producer so the consumer genuinely parks */
      if ((i % 64) == 63 || rng_range(&pause_rng, 4) == 0)
         nanosleep(&pause, NULL);
   }

   pthread_join(tid, NULL);
   CHECK(c.error == 0, "consumer reported an error");
   CHECK(c.consumed == SP_ITEMS, "consumed %u != %u", c.consumed,
         (unsigned)SP_ITEMS);
   CHECK(entries == SP_ITEMS, "produced wrong count");

   wake_destroy(c.wake);
   free(buf);
   test_region_fini(&region);
}

/* a signal posted before the wait must not be lost */
static void
test_signal_before_wait(void)
{
   render_wake_t w = wake_create();
   CHECK(w != RENDER_WAKE_NULL, "wake_create failed");

   render_wake_signal(w);
   CHECK(render_wake_wait(w, 0) == true, "signalled wait returned timeout");

   /* the primitive is counting: exactly one pending signal */
   CHECK(render_wake_wait(w, 0) == false,
         "phantom second signal observed");

   /* two signals coalesce into at least one immediate return */
   render_wake_signal(w);
   render_wake_signal(w);
   CHECK(render_wake_wait(w, 0) == true, "double signal lost");

   wake_destroy(w);
}

/* ------------------------------------------------------------------ */
/* cross-process conditional wake (posix_spawn + registered ports)      */

#define XPROC_ITEMS 200000
#define XPROC_MAX_PAYLOAD 2048

#if defined(__APPLE__)
/* spawn-time handoff: two semaphores in the child's registered ports */
#define XPROC_PORT_MAIN 0
#define XPROC_PORT_COMPL 1
#define XPROC_PORT_COUNT 2
#endif

struct xproc_shared {
   struct render_ring_control ctl;
   /* the region is the control block plus the main ring laid out
    * manually: control + 1 MiB ring */
   uint8_t ring_data[RENDER_RING_MAIN_SIZE];
};

#define XPROC_REGION_SIZE ((uint32_t)sizeof(struct xproc_shared))

static struct xproc_shared *
xproc_region_create(int *fd_out)
{

   char name[64];
   snprintf(name, sizeof(name), "/virgl-ring-test-%d", (int)getpid());
   const int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
   if (fd < 0)
      return NULL;
   shm_unlink(name);
   if (ftruncate(fd, XPROC_REGION_SIZE)) {
      close(fd);
      return NULL;
   }
   void *p = mmap(NULL, XPROC_REGION_SIZE, PROT_READ | PROT_WRITE,
                  MAP_SHARED, fd, 0);
   if (p == MAP_FAILED) {
      close(fd);
      return NULL;
   }
   memset(p, 0, XPROC_REGION_SIZE);
   *fd_out = fd;
   return p;
}

static struct render_ring_buf
xproc_main_buf(struct xproc_shared *sh)
{
   return (struct render_ring_buf){
      .head = &sh->ctl.main_head.value,
      .tail = &sh->ctl.main_tail.value,
      .data = sh->ring_data,
      .size = RENDER_RING_MAIN_SIZE,
   };
}

struct xproc_consumer {
   struct render_ring_buf *ring;
   uint32_t head;
};

static bool
xproc_main_has_work(void *opaque)
{
   const struct xproc_consumer *c = opaque;
   return render_ring_has_entries(c->ring, c->head);
}

struct xproc_compl {
   struct render_ring_control *ctl;
   uint32_t *last;
};

static bool
xproc_compl_has_work(void *opaque)
{
   const struct xproc_compl *c = opaque;
   return atomic_load_explicit(&c->ctl->timelines[0], memory_order_acquire) !=
          *c->last;
}

/* child: consume XPROC_ITEMS ordered entries off the main ring; for each
 * consumed item publish a completion seqno (the reverse direction) */
static int
xproc_child(struct xproc_shared *sh, render_wake_t main_wake,
            render_wake_t compl_wake)
{
   struct render_ring_buf ring = xproc_main_buf(sh);
   struct rng sizes = { .s = 0x9e3779b97f4a7c15ull };
   struct xproc_consumer consumer = { .ring = &ring, .head = 0 };

   for (uint32_t i = 0; i < XPROC_ITEMS;) {
      uint32_t op, size;
      const uint8_t *payload;
      const enum render_ring_read_result res =
         render_ring_read(&ring, &consumer.head, &op, &payload, &size);
      if (res == RENDER_RING_READ_CORRUPT) {
         fprintf(stderr, "child: corrupt entry at %u\n", consumer.head);
         return 2;
      }
      if (res == RENDER_RING_READ_OK) {
         const uint32_t expect =
            (uint32_t)sizeof(uint32_t) +
            rng_range(&sizes, XPROC_MAX_PAYLOAD + 1 - (uint32_t)sizeof(uint32_t));
         if (op != RENDER_RING_OP_SUBMIT_CMD || size != expect) {
            fprintf(stderr, "child: entry %u size %u expected %u\n", i, size,
                    expect);
            return 2;
         }
         uint32_t seq;
         memcpy(&seq, payload, sizeof(seq));
         if (seq != i) {
            fprintf(stderr, "child: out-of-order seq %u expected %u\n", seq,
                    i);
            return 2;
         }
         for (uint32_t j = sizeof(seq); j < size; j++) {
            if (payload[j] != payload_byte(i, j)) {
               fprintf(stderr, "child: entry %u corrupted at byte %u\n", i,
                       j);
               return 2;
            }
         }
         i++;
         render_ring_commit(&ring, consumer.head);

         /* completion direction: publish seqno, conditional wake */
         atomic_store_explicit(&sh->ctl.timelines[0], i,
                               memory_order_release);
         render_ring_producer_wake(&sh->ctl.vmm_state.value, compl_wake,
                                   &sh->ctl.stats_worker.v.fence_notifies);
         continue;
      }

      const enum render_ring_wait_result wr = render_ring_consumer_wait(
         &sh->ctl.main_state.value, main_wake, xproc_main_has_work,
         &consumer, 0, 3000ll * 1000000 /* 3 s */,
         &sh->ctl.stats_worker.v.main_sleeps);
      if (wr == RENDER_RING_WAIT_ERROR)
         return 2;
      if (wr == RENDER_RING_WAIT_TIMEOUT) {
         if (render_ring_has_entries(&ring, consumer.head)) {
            fprintf(stderr, "child: lost wakeup at %u entries\n", i);
            return 2;
         }
         fprintf(stderr, "child: timed out waiting for entry %u\n", i);
         return 2;
      }
   }
   return 0;
}

#if defined(__APPLE__)
static int
xproc_child_main(int shmem_fd)
{
   mach_port_array_t ports = NULL;
   mach_msg_type_number_t count = 0;
   if (mach_ports_lookup(mach_task_self(), &ports, &count) != KERN_SUCCESS ||
       count < XPROC_PORT_COUNT || !ports[XPROC_PORT_MAIN] ||
       !ports[XPROC_PORT_COMPL]) {
      fprintf(stderr, "child: missing registered ports (count %u)\n",
              (unsigned)count);
      return 2;
   }
   /* ports array stays mapped for the child's lifetime */


   struct stat st;
   if (fstat(shmem_fd, &st) || st.st_size < (off_t)XPROC_REGION_SIZE) {
      fprintf(stderr, "child: bad shmem fd\n");
      return 2;
   }
   void *p = mmap(NULL, XPROC_REGION_SIZE, PROT_READ | PROT_WRITE,
                  MAP_SHARED, shmem_fd, 0);
   if (p == MAP_FAILED) {
      fprintf(stderr, "child: mmap failed\n");
      return 2;
   }
   return xproc_child((struct xproc_shared *)p, ports[XPROC_PORT_MAIN],
                      ports[XPROC_PORT_COMPL]);
}
#endif /* __APPLE__ */

static int
xproc_parent(struct xproc_shared *sh, render_wake_t main_wake,
             render_wake_t compl_wake, pid_t child)
{
   struct render_ring_buf ring = xproc_main_buf(sh);
   struct rng sizes = { .s = 0x9e3779b97f4a7c15ull };
   struct rng burst = { .s = 0xd1b54a32d192ed03ull };
   uint8_t *buf = malloc(XPROC_MAX_PAYLOAD + 8);

   const struct timespec backoff = { .tv_sec = 0, .tv_nsec = 20000 };

   uint32_t produced = 0;
   uint32_t last_compl = 0;
   struct xproc_compl compl_ctx = { .ctl = &sh->ctl, .last = &last_compl };

   while (produced < XPROC_ITEMS) {
      const uint32_t burst_len = 1 + rng_range(&burst, 64);
      for (uint32_t b = 0; b < burst_len && produced < XPROC_ITEMS;
           b++, produced++) {
         const uint32_t size =
            (uint32_t)sizeof(uint32_t) +
            rng_range(&sizes, XPROC_MAX_PAYLOAD + 1 - (uint32_t)sizeof(uint32_t));
         memcpy(buf, &produced, sizeof(produced));
         for (uint32_t j = sizeof(uint32_t); j < size; j++)
            buf[j] = payload_byte(produced, j);
         while (!render_ring_push(&ring, RENDER_RING_OP_SUBMIT_CMD, buf,
                                  size))
            nanosleep(&backoff, NULL);
         render_ring_producer_wake(&sh->ctl.main_state.value, main_wake,
                                   &sh->ctl.stats_vmm.v.main_wakes);
      }
      /* pace between bursts so the consumer parks: bursty production
       * should yield far fewer wakes than items, but not zero */
      const uint64_t pause_ns = (uint64_t)rng_range(&burst, 4) * 50000;
      if (pause_ns) {
         const struct timespec pause = {
            .tv_sec = 0,
            .tv_nsec = (long)pause_ns,
         };
         nanosleep(&pause, NULL);
      }
      /* opportunistically harvest completions while producing */
      last_compl = atomic_load_explicit(&sh->ctl.timelines[0],
                                        memory_order_acquire);
   }

   /* drain completions with the VMM-side conditional wake */
   while (last_compl < XPROC_ITEMS) {
      const enum render_ring_wait_result wr = render_ring_consumer_wait(
         &sh->ctl.vmm_state.value, compl_wake, xproc_compl_has_work,
         &compl_ctx, 0, 3000ll * 1000000,
         &sh->ctl.stats_vmm.v.vmm_sleeps);
      if (wr == RENDER_RING_WAIT_ERROR)
         goto fail;
      if (wr == RENDER_RING_WAIT_TIMEOUT) {
         const uint32_t now = atomic_load_explicit(&sh->ctl.timelines[0],
                                                   memory_order_acquire);
         if (now != last_compl) {
            last_compl = now;
            continue;
         }
         fprintf(stderr, "parent: lost completion wakeup at %u\n",
                 last_compl);
         goto fail;
      }
      last_compl = atomic_load_explicit(&sh->ctl.timelines[0],
                                        memory_order_acquire);
   }

   int status = 0;
   waitpid(child, &status, 0);
   if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      fprintf(stderr, "parent: child failed (status %d)\n", status);
      goto fail;
   }

   /* bursty production: wakes must be far below the item count */
   const uint64_t wakes =
      atomic_load(&sh->ctl.stats_vmm.v.main_wakes);
   const uint64_t compl_wakes =
      atomic_load(&sh->ctl.stats_worker.v.fence_notifies);
   printf("cross-process: %u items, %llu main wakes, %llu completion "
          "wakes\n",
          (unsigned)XPROC_ITEMS, (unsigned long long)wakes,
          (unsigned long long)compl_wakes);
   CHECK(wakes > 0, "no conditional wakes observed");
   CHECK(wakes < XPROC_ITEMS, "wakes %llu not below item count %u",
         (unsigned long long)wakes, (unsigned)XPROC_ITEMS);
   CHECK(compl_wakes < XPROC_ITEMS,
         "completion wakes %llu not below item count",
         (unsigned long long)compl_wakes);

   free(buf);
   return 0;

fail:
   free(buf);
   kill(child, SIGKILL);
   waitpid(child, NULL, 0);
   return 1;
}

static int
test_cross_process(void)
{
#if defined(__APPLE__)
   int shm_fd = -1;
   struct xproc_shared *sh = xproc_region_create(&shm_fd);
   CHECK(sh != NULL, "region create failed");
   if (!sh)
      return 1;

   render_wake_t main_wake = wake_create();
   render_wake_t compl_wake = wake_create();
   CHECK(main_wake != RENDER_WAKE_NULL && compl_wake != RENDER_WAKE_NULL,
         "wake_create failed");

   /* child executable: self, or an x86_64 build for the Rosetta run */
   char self[4096];
   uint32_t self_len = sizeof(self);
   const char *child_path = getenv("TEST_RENDER_RING_CHILD");
   if (!child_path) {
      if (_NSGetExecutablePath(self, &self_len))
         goto fail;
      child_path = self;
   }

   char fd_str[16];
   snprintf(fd_str, sizeof(fd_str), "%d", shm_fd);
   char *const argv[] = {
      (char *)child_path, (char *)"--ring-child", fd_str, NULL,
   };

   posix_spawnattr_t attr;
   if (posix_spawnattr_init(&attr))
      goto fail;
   posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
   const mach_port_t ports[XPROC_PORT_COUNT] = { main_wake, compl_wake };
   if (posix_spawnattr_set_registered_ports_np(&attr, ports,
                                               XPROC_PORT_COUNT)) {
      fprintf(stderr, "registered ports failed\n");
      posix_spawnattr_destroy(&attr);
      goto fail;
   }

   posix_spawn_file_actions_t fa;
   if (posix_spawn_file_actions_init(&fa)) {
      posix_spawnattr_destroy(&attr);
      goto fail;
   }
   posix_spawn_file_actions_addinherit_np(&fa, STDIN_FILENO);
   posix_spawn_file_actions_addinherit_np(&fa, STDOUT_FILENO);
   posix_spawn_file_actions_addinherit_np(&fa, STDERR_FILENO);
   posix_spawn_file_actions_addinherit_np(&fa, shm_fd);

   pid_t child;
   const int err = posix_spawn(&child, child_path, &fa, &attr, argv, NULL);
   posix_spawn_file_actions_destroy(&fa);
   posix_spawnattr_destroy(&attr);
   if (err) {
      fprintf(stderr, "posix_spawn(%s): %s\n", child_path, strerror(err));
      goto fail;
   }

   const int ret = xproc_parent(sh, main_wake, compl_wake, child);
   wake_destroy(main_wake);
   wake_destroy(compl_wake);
   close(shm_fd);
   munmap(sh, XPROC_REGION_SIZE);
   return ret;

fail:
   wake_destroy(main_wake);
   wake_destroy(compl_wake);
   if (shm_fd >= 0)
      close(shm_fd);
   munmap(sh, XPROC_REGION_SIZE);
   failures++;
   return 1;
#else
   /* Linux: fork + eventfd; the shared mapping survives exec-free
    * spawn through MAP_ANON */
   struct xproc_shared *sh =
      mmap(NULL, XPROC_REGION_SIZE, PROT_READ | PROT_WRITE,
           MAP_SHARED | MAP_ANON, -1, 0);
   CHECK(sh != MAP_FAILED, "region create failed");
   if (sh == MAP_FAILED)
      return 1;
   memset(sh, 0, XPROC_REGION_SIZE);

   render_wake_t main_wake = wake_create();
   render_wake_t compl_wake = wake_create();
   CHECK(main_wake != RENDER_WAKE_NULL && compl_wake != RENDER_WAKE_NULL,
         "wake_create failed");

   const pid_t child = fork();
   if (child < 0) {
      failures++;
      return 1;
   }
   if (child == 0)
      _exit(xproc_child(sh, main_wake, compl_wake));

   const int ret = xproc_parent(sh, main_wake, compl_wake, child);
   wake_destroy(main_wake);
   wake_destroy(compl_wake);
   munmap(sh, XPROC_REGION_SIZE);
   return ret;
#endif
}

/* ------------------------------------------------------------------ */
/* producer bail-out when the worker is dead                            */

static int
test_dead_worker_bailout(void)
{
   struct test_region region;
   test_region_init(&region, false);

   /* producer-side contract: once worker_dead is set, pushes must bail
    * with an error instead of hanging on a ring that will never drain */
   _Atomic bool worker_dead = true;
   struct render_ring_buf ring = test_main_buf(&region);
   uint8_t *scratch = calloc(1, 65536);
   while (render_ring_push(&ring, RENDER_RING_OP_SUBMIT_CMD, scratch, 65528)) {
   }
   while (render_ring_push(&ring, RENDER_RING_OP_SUBMIT_CMD, scratch, 8)) {
   }
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
main(int argc, char **argv)
{
#if defined(__APPLE__)
   if (argc >= 3 && !strcmp(argv[1], "--ring-child"))
      return xproc_child_main(atoi(argv[2]));
#endif
   (void)argc;
   (void)argv;

   test_encode_decode();
   test_lat_layout();
   test_signal_before_wait();
   test_conditional_wake();
   if (test_dead_worker_bailout())
      failures++;
   if (test_cross_process())
      failures++;

   if (failures) {
      fprintf(stderr, "%d check(s) failed\n", failures);
      return 1;
   }
   printf("test_render_ring: all checks passed\n");
   return 0;
}
