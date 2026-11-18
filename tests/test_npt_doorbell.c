/*
 * Copyright 2026
 * SPDX-License-Identifier: MIT
 */

/* Tests for the MMIO doorbell: the proxy slot table (register/ring/
 * unregister lifetime), the shared doorbell mask, the conditional wake,
 * and the worker-side mask drain. */

#include "server/render_ring.h"
#include "proxy/proxy_doorbell.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <mach/mach.h>
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

static struct render_ring_control *
ctl_create(void)
{
   struct render_ring_control *ctl =
      aligned_alloc(RENDER_RING_CACHELINE, sizeof(*ctl));
   if (ctl)
      render_ring_control_init(ctl);
   return ctl;
}

/* ------------------------------------------------------------------ */

static void
test_unknown_ctx(void)
{
   CHECK(proxy_doorbell_ring(0, 0) == -EINVAL, "ctx_id 0 accepted");
   CHECK(proxy_doorbell_ring(7, 0) == -EINVAL, "unregistered ctx accepted");
   CHECK(proxy_doorbell_ring(PROXY_DOORBELL_SLOT_COUNT, 0) == -EINVAL,
         "ctx_id 256 accepted");
   CHECK(proxy_doorbell_ring(1, RENDER_RING_DOORBELL_MAX_RINGS) == -EINVAL,
         "out-of-range ring id accepted");
}

static void
test_mask_and_conditional_wake(void)
{
   struct render_ring_control *ctl = ctl_create();
   CHECK(ctl != NULL, "ctl alloc failed");
   render_wake_t wake = wake_create();
   CHECK(wake != RENDER_WAKE_NULL, "wake create failed");
   if (!ctl || wake == RENDER_WAKE_NULL)
      return;

   const uint32_t ctx_id = 7;
   proxy_doorbell_register(ctx_id, ctl, wake);

   /* consumer ACTIVE: bit is set but no signal is sent */
   atomic_store(&ctl->oob_state.value, RENDER_RING_ACTIVE);
   CHECK(proxy_doorbell_ring(ctx_id, 42) == 0, "ring failed");
   CHECK(atomic_load(&ctl->doorbell.mask[0]) == (UINT64_C(1) << 42),
         "bit 42 not set");
   CHECK(render_wake_wait(wake, 1000000) == false,
         "spurious wake while consumer ACTIVE");

   /* consumer SLEEPING: a doorbell must signal it, exactly once per
    * SLEEPING->WAKING transition */
   atomic_store(&ctl->oob_state.value, RENDER_RING_SLEEPING);
   CHECK(proxy_doorbell_ring(ctx_id, 3) == 0, "ring failed");
   CHECK(atomic_load(&ctl->oob_state.value) == RENDER_RING_WAKING,
         "state not WAKING");
   CHECK(atomic_load(&ctl->doorbell.mask[0]) == ((UINT64_C(1) << 42) | 8),
         "bit 3 not set");
   CHECK(render_wake_wait(wake, 10000000) == true,
         "missing wake while consumer SLEEPING");

   /* second ring while WAKING must not double-signal */
   CHECK(proxy_doorbell_ring(ctx_id, 5) == 0, "ring failed");
   CHECK(render_wake_wait(wake, 1000000) == false, "double signal");

   proxy_doorbell_unregister(ctx_id);
   CHECK(proxy_doorbell_ring(ctx_id, 0) == -EINVAL,
         "ctx still live after unregister");

   wake_destroy(wake);
   free(ctl);
}

/* ------------------------------------------------------------------ */

struct hammer_args {
   uint32_t ctx_id;
   uint32_t rounds;
   uint64_t accepted;
   uint64_t rejected;
};

static void *
hammer_ring(void *arg)
{
   struct hammer_args *a = arg;
   for (uint32_t i = 0; i < a->rounds; i++) {
      if (proxy_doorbell_ring(a->ctx_id, i % 64) == 0)
         a->accepted++;
      else
         a->rejected++;
   }
   return NULL;
}

/* Ringers from many threads must not fault while unregister frees the
 * slot's view of the shared region (the mask itself outlives the test,
 * emulating the shm being unmapped only after the write lock). */
static void
test_concurrent_unregister(void)
{
   for (uint32_t iter = 0; iter < 100; iter++) {
      struct render_ring_control *ctl = ctl_create();
      render_wake_t wake = wake_create();
      CHECK(ctl && wake != RENDER_WAKE_NULL, "alloc failed");
      if (!ctl || wake == RENDER_WAKE_NULL)
         return;
      atomic_store(&ctl->oob_state.value, RENDER_RING_ACTIVE);

      const uint32_t ctx_id = 9;
      proxy_doorbell_register(ctx_id, ctl, wake);

      pthread_t th[8];
      struct hammer_args args[8];
      for (uint32_t t = 0; t < 8; t++) {
         args[t] = (struct hammer_args){
            .ctx_id = ctx_id, .rounds = 500,
         };
         pthread_create(&th[t], NULL, hammer_ring, &args[t]);
      }
      proxy_doorbell_unregister(ctx_id);
      for (uint32_t t = 0; t < 8; t++)
         pthread_join(th[t], NULL);

      proxy_doorbell_unregister(ctx_id);
      wake_destroy(wake);
      free(ctl);
   }
}

/* ------------------------------------------------------------------ */

struct consumer_args {
   struct render_ring_control *ctl;
   render_wake_t wake;
   _Atomic bool stop;
   uint64_t drained;
};

static bool
mask_has_work(void *opaque)
{
   return render_ring_doorbell_pending(opaque);
}

static void
count_cb(void *opaque, uint32_t ring_id)
{
   (void)ring_id;
   (*(uint64_t *)opaque)++;
}

static void *
consumer_fn(void *arg)
{
   struct consumer_args *a = arg;
   while (!atomic_load_explicit(&a->stop, memory_order_acquire) ||
          render_ring_doorbell_pending(a->ctl)) {
      render_ring_doorbell_drain(a->ctl, count_cb, &a->drained);
      render_ring_consumer_wait(&a->ctl->oob_state.value, a->wake,
                                mask_has_work, a->ctl, 0, 1000000, NULL);
   }
   return NULL;
}

/* Producer rings while the consumer races to sleep: every bit must be
 * delivered exactly once. */
static void
test_no_lost_doorbell(void)
{
   struct render_ring_control *ctl = ctl_create();
   render_wake_t wake = wake_create();
   CHECK(ctl && wake != RENDER_WAKE_NULL, "alloc failed");
   if (!ctl || wake == RENDER_WAKE_NULL)
      return;

   const uint32_t ctx_id = 11;
   proxy_doorbell_register(ctx_id, ctl, wake);

   struct consumer_args cargs = {
      .ctl = ctl, .wake = wake,
   };
   pthread_t cth;
   pthread_create(&cth, NULL, consumer_fn, &cargs);

   /* The mask collapses repeated rings on the same id, so the
    * meaningful invariant is that every distinct bit is delivered:
    * ring each id once while the consumer cycles drain -> sleep -> recheck. */
   for (uint32_t i = 0; i < RENDER_RING_DOORBELL_MAX_RINGS; i++)
      CHECK(proxy_doorbell_ring(ctx_id, i) == 0, "ring failed");

   atomic_store(&cargs.stop, true);
   proxy_doorbell_ring(ctx_id, 0); /* kick it out of the wait */
   pthread_join(cth, NULL);

   CHECK(cargs.drained == RENDER_RING_DOORBELL_MAX_RINGS,
         "drained %llu of %u doorbells",
         (unsigned long long)cargs.drained, RENDER_RING_DOORBELL_MAX_RINGS);

   proxy_doorbell_unregister(ctx_id);
   wake_destroy(wake);
   free(ctl);
}

/* ------------------------------------------------------------------ */

struct bit_rec {
   uint32_t ids[RENDER_RING_DOORBELL_MAX_RINGS];
   uint32_t count;
};

static void
rec_cb(void *opaque, uint32_t ring_id)
{
   struct bit_rec *r = opaque;
   if (r->count < RENDER_RING_DOORBELL_MAX_RINGS)
      r->ids[r->count++] = ring_id;
}

static void
test_drain_invokes_per_bit(void)
{
   struct render_ring_control *ctl = ctl_create();
   CHECK(ctl != NULL, "ctl alloc failed");
   if (!ctl)
      return;

   const uint32_t want[] = {0, 1, 63, 64, 65, 127};
   const uint32_t nwant = sizeof(want) / sizeof(want[0]);
   for (uint32_t i = 0; i < nwant; i++)
      atomic_fetch_or(&ctl->doorbell.mask[want[i] >> 6],
                      UINT64_C(1) << (want[i] & 63));

   struct bit_rec rec = { .count = 0 };
   render_ring_doorbell_drain(ctl, rec_cb, &rec);

   CHECK(rec.count == nwant, "got %u callbacks, want %u", rec.count, nwant);
   for (uint32_t i = 0; i < nwant && i < rec.count; i++)
      CHECK(rec.ids[i] == want[i], "bit %u delivered as %u", want[i],
            rec.ids[i]);
   CHECK(!render_ring_doorbell_pending(ctl), "mask not drained");

   free(ctl);
}

int
main(void)
{
   test_unknown_ctx();
   test_mask_and_conditional_wake();
   test_concurrent_unregister();
   test_no_lost_doorbell();
   test_drain_invokes_per_bit();

   if (failures) {
      fprintf(stderr, "test_npt_doorbell: %d failures\n", failures);
      return 1;
   }
   fprintf(stderr, "test_npt_doorbell: all checks passed\n");
   return 0;
}
