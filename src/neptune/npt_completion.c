/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#include "npt_completion.h"

#include <stdlib.h>

static void
npt_evt_overflow_flush_locked(struct npt_completion_producer *prod,
                              struct npt_context_status_page *page);

void
npt_completion_producer_init(struct npt_completion_producer *prod)
{
   mtx_init(&prod->mtx, mtx_plain);
   prod->tail = 0;
   prod->ovf_head = NULL;
   prod->ovf_tail = NULL;
}

void
npt_completion_producer_fini(struct npt_completion_producer *prod)
{
   struct npt_evt_overflow_entry *o = prod->ovf_head;
   while (o) {
      struct npt_evt_overflow_entry *next = o->next;
      free(o);
      o = next;
   }
   prod->ovf_head = NULL;
   prod->ovf_tail = NULL;
   mtx_destroy(&prod->mtx);
}

/* Caller holds prod->mtx.  Returns false when the ring has no space. */
static bool
npt_evt_push_locked(struct npt_completion_producer *prod,
                    struct npt_context_status_page *page,
                    uint64_t token)
{
   const uint64_t t = prod->tail;
   uint64_t head = atomic_load_explicit(&page->evt_head,
                                        memory_order_acquire);
   /* the guest head is untrusted: clamp it so t - head <= SIZE,
    * which also bounds the space computation */
   if (t - head > NPT_COMPLETION_QUEUE_SIZE)
      head = t - NPT_COMPLETION_QUEUE_SIZE;
   if (t - head >= NPT_COMPLETION_QUEUE_SIZE)
      return false;

   atomic_store_explicit(&page->evt_tokens[t % NPT_COMPLETION_QUEUE_SIZE],
                         token, memory_order_relaxed);
   atomic_store_explicit(&page->evt_tail, t + 1, memory_order_release);
   prod->tail = t + 1;
   return true;
}

static void
npt_evt_overflow_flush_locked(struct npt_completion_producer *prod,
                              struct npt_context_status_page *page)
{
   while (prod->ovf_head) {
      struct npt_evt_overflow_entry *o = prod->ovf_head;
      if (!npt_evt_push_locked(prod, page, o->token))
         break;
      prod->ovf_head = o->next;
      if (!prod->ovf_head)
         prod->ovf_tail = NULL;
      free(o);
   }
}

bool
npt_completion_flush(struct npt_completion_producer *prod,
                     struct npt_context_status_page *page)
{
   mtx_lock(&prod->mtx);
   const uint64_t before = prod->tail;
   npt_evt_overflow_flush_locked(prod, page);
   const bool published = prod->tail != before;
   mtx_unlock(&prod->mtx);
   return published;
}

bool
npt_completion_publish(struct npt_completion_producer *prod,
                       struct npt_context_status_page *page,
                       uint64_t token)
{
   bool ok = true;

   /* Flush-then-push, no sleeping: the fire path is latency-sensitive,
    * and the sync-queue poll cycle retries the FIFO periodically. */
   mtx_lock(&prod->mtx);
   npt_evt_overflow_flush_locked(prod, page);

   if (!npt_evt_push_locked(prod, page, token)) {
      struct npt_evt_overflow_entry *o = malloc(sizeof(*o));
      if (o) {
         o->token = token;
         o->next = NULL;
         if (prod->ovf_tail)
            prod->ovf_tail->next = o;
         else
            prod->ovf_head = o;
         prod->ovf_tail = o;
      } else {
         ok = false;
      }
   }
   mtx_unlock(&prod->mtx);
   return ok;
}

bool
npt_completion_maybe_irq(struct npt_context_status_page *page)
{
   atomic_thread_fence(memory_order_seq_cst);
   return atomic_load_explicit(&page->guest_waiters,
                               memory_order_seq_cst) != 0 &&
          atomic_exchange(&page->irq_pending, 1) == 0;
}
