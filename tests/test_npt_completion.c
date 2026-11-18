/*
 * Copyright 2026
 * SPDX-License-Identifier: MIT
 *
 * In-memory test of the worker-side completion producer: in-order
 * delivery across overflow, the untrusted guest head clamp, and the
 * irq_pending handshake with guest_waiters.
 */

#include <stdatomic.h>
#include <stdio.h>
#include <sys/mman.h>

#include "neptune/npt_completion.h"

static int failures;

#define CHECK(cond, what)                                                \
   do {                                                                  \
      if (!(cond)) {                                                     \
         fprintf(stderr, "test_npt_completion: FAIL %s\n", what);        \
         failures++;                                                     \
      }                                                                  \
   } while (0)

static struct npt_context_status_page *
test_page(void)
{
   struct npt_context_status_page *page =
      mmap(NULL, NPT_CONTEXT_STATUS_PAGE_SIZE, PROT_READ | PROT_WRITE,
           MAP_SHARED | MAP_ANON, -1, 0);
   if (page == MAP_FAILED)
      return NULL;
   memset(page, 0, NPT_CONTEXT_STATUS_PAGE_SIZE);
   return page;
}

static uint64_t
page_tail(const struct npt_context_status_page *page)
{
   return atomic_load_explicit((_Atomic uint64_t *)&page->evt_tail,
                               memory_order_acquire);
}

static uint64_t
page_token(const struct npt_context_status_page *page, uint64_t t)
{
   return atomic_load_explicit(
      (_Atomic uint64_t *)&page->evt_tokens[t % NPT_COMPLETION_QUEUE_SIZE],
      memory_order_acquire);
}

static void
test_in_order_across_overflow(void)
{
   struct npt_context_status_page *page = test_page();
   struct npt_completion_producer prod;
   npt_completion_producer_init(&prod);

   const uint64_t over = 8;
   const uint64_t total = NPT_COMPLETION_QUEUE_SIZE + over;
   for (uint64_t i = 0; i < total; i++)
      CHECK(npt_completion_publish(&prod, page, 100 + i), "publish");

   CHECK(page_tail(page) == NPT_COMPLETION_QUEUE_SIZE,
         "ring full, rest overflowed");
   for (uint64_t t = 0; t < NPT_COMPLETION_QUEUE_SIZE; t++)
      if (page_token(page, t) != 100 + t) {
         CHECK(false, "ring token order");
         break;
      }

   /* drain half, then publish one more: the overflow FIFO flushes
    * in order ahead of it */
   atomic_store_explicit((_Atomic uint64_t *)&page->evt_head,
                         NPT_COMPLETION_QUEUE_SIZE / 2,
                         memory_order_release);
   CHECK(npt_completion_publish(&prod, page, 999), "post-drain publish");
   for (uint64_t t = 0; t < over; t++)
      if (page_token(page, NPT_COMPLETION_QUEUE_SIZE + t) !=
          100 + NPT_COMPLETION_QUEUE_SIZE + t) {
         CHECK(false, "overflow order preserved");
         break;
      }
   CHECK(page_token(page, NPT_COMPLETION_QUEUE_SIZE + over) == 999,
         "new publish after overflow");
   CHECK(page_tail(page) == NPT_COMPLETION_QUEUE_SIZE + over + 1,
         "tail advanced");

   npt_completion_producer_fini(&prod);
   munmap(page, NPT_CONTEXT_STATUS_PAGE_SIZE);
}

static void
test_flush_delivers_overflow(void)
{
   struct npt_context_status_page *page = test_page();
   struct npt_completion_producer prod;
   npt_completion_producer_init(&prod);

   /* Fill the ring and overflow 4 tokens. */
   for (uint64_t i = 0; i < NPT_COMPLETION_QUEUE_SIZE; i++)
      CHECK(npt_completion_publish(&prod, page, i + 1), "fill");
   for (uint64_t i = 0; i < 4; i++)
      CHECK(npt_completion_publish(&prod, page, 5000 + i), "overflow");
   CHECK(page_tail(page) == NPT_COMPLETION_QUEUE_SIZE, "ring full");
   CHECK(npt_completion_has_overflow(&prod), "overflow pending");

   /* Without the guest head advancing, flush publishes nothing. */
   CHECK(!npt_completion_flush(&prod, page), "flush blocked by full ring");

   /* Guest drains 4: flush alone delivers the overflowed tokens. */
   atomic_store_explicit((_Atomic uint64_t *)&page->evt_head, 4,
                         memory_order_release);
   CHECK(npt_completion_flush(&prod, page), "flush published");
   CHECK(!npt_completion_has_overflow(&prod), "overflow drained");
   CHECK(page_tail(page) == NPT_COMPLETION_QUEUE_SIZE + 4,
         "tail advanced by overflow count");
   for (uint64_t t = 0; t < 4; t++)
      if (page_token(page, NPT_COMPLETION_QUEUE_SIZE + t) != 5000 + t) {
         CHECK(false, "flushed tokens in order");
         break;
      }
   CHECK(!npt_completion_flush(&prod, page), "empty flush is a no-op");

   npt_completion_producer_fini(&prod);
   munmap(page, NPT_CONTEXT_STATUS_PAGE_SIZE);
}

static void
test_bogus_head_clamped(void)
{
   struct npt_context_status_page *page = test_page();
   struct npt_completion_producer prod;
   npt_completion_producer_init(&prod);

   for (uint64_t i = 0; i < 16; i++)
      CHECK(npt_completion_publish(&prod, page, i + 1), "publish");
   const uint64_t tail = page_tail(page);

   /* head far in the future must not corrupt state: the publish just
    * overflows instead of writing outside the window */
   atomic_store_explicit((_Atomic uint64_t *)&page->evt_head,
                         tail + (1ull << 40), memory_order_release);
   CHECK(npt_completion_publish(&prod, page, 77), "bogus-head publish");
   CHECK(page_tail(page) == tail, "tail unchanged under bogus head");

   /* a sane head flushes the overflowed token first, in order */
   atomic_store_explicit((_Atomic uint64_t *)&page->evt_head, tail,
                         memory_order_release);
   CHECK(npt_completion_publish(&prod, page, 200), "flush publish");
   CHECK(page_token(page, tail) == 77, "overflowed token in order");
   CHECK(page_token(page, tail + 1) == 200, "new publish after flush");

   /* fill to capacity with a stale head: the rest overflows */
   atomic_store_explicit((_Atomic uint64_t *)&page->evt_head, 0,
                         memory_order_release);
   for (uint64_t i = 0; i < NPT_COMPLETION_QUEUE_SIZE; i++)
      CHECK(npt_completion_publish(&prod, page, 1000 + i),
            "fill to capacity");
   CHECK(page_tail(page) == NPT_COMPLETION_QUEUE_SIZE,
         "stale head bounds space");

   /* drain fully, then one more publish flushes the overflowed
    * remainder in order ahead of it */
   atomic_store_explicit((_Atomic uint64_t *)&page->evt_head,
                         NPT_COMPLETION_QUEUE_SIZE, memory_order_release);
   CHECK(npt_completion_publish(&prod, page, 9999), "drain publish");
   const uint64_t in_ring = NPT_COMPLETION_QUEUE_SIZE - (tail + 2);
   CHECK(page_token(page, NPT_COMPLETION_QUEUE_SIZE) ==
            1000 + in_ring,
         "overflow remainder in order");
   CHECK(page_token(page, NPT_COMPLETION_QUEUE_SIZE + tail + 2) ==
            9999,
         "publish lands after overflow flush");

   npt_completion_producer_fini(&prod);
   munmap(page, NPT_CONTEXT_STATUS_PAGE_SIZE);
}

static void
test_irq_handshake(void)
{
   struct npt_context_status_page *page = test_page();

   CHECK(!npt_completion_maybe_irq(page), "no irq without waiters");
   CHECK(atomic_load(&page->irq_pending) == 0, "irq_pending stays 0");

   atomic_store(&page->guest_waiters, 1);
   CHECK(npt_completion_maybe_irq(page), "irq when waiter exists");
   CHECK(atomic_load(&page->irq_pending) == 1, "irq_pending latched");
   CHECK(!npt_completion_maybe_irq(page), "irq only once until cleared");

   atomic_store(&page->irq_pending, 0);
   CHECK(npt_completion_maybe_irq(page), "irq again after clear");

   atomic_store(&page->guest_waiters, 0);
   atomic_store(&page->irq_pending, 0);
   CHECK(!npt_completion_maybe_irq(page), "no irq after waiters leave");

   munmap(page, NPT_CONTEXT_STATUS_PAGE_SIZE);
}

int
main(void)
{
   test_in_order_across_overflow();
   test_bogus_head_clamped();
   test_flush_delivers_overflow();
   test_irq_handshake();

   if (failures) {
      fprintf(stderr, "test_npt_completion: %d check(s) failed\n",
              failures);
      return 1;
   }
   printf("test_npt_completion: all checks passed\n");
   return 0;
}
