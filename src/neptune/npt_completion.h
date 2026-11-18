/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Worker-side producer for the completion queue in the context status
 * page (npt_context_status_page v2).  Self-contained so tests can link
 * it without the rest of the renderer.
 *
 * Trust model: the worker trusts only its private tail.  The guest's
 * evt_head is read solely for space accounting and clamped so that
 * t - head never exceeds NPT_COMPLETION_QUEUE_SIZE -- a bogus head can
 * stall delivery but can never corrupt producer state or overrun the
 * token array.
 */

#ifndef NPT_COMPLETION_H
#define NPT_COMPLETION_H

#include <stdbool.h>
#include <stdint.h>

#include "c11/threads.h"

#include "neptune-protocol/npt_protocol_transport.h"

struct npt_evt_overflow_entry {
   struct npt_evt_overflow_entry *next;
   uint64_t token;
};

struct npt_completion_producer {
   /* Publish is thread-safe: event completions can fire on several
    * host threads at once. */
   mtx_t mtx;
   /* Worker-private producer tail; evt_tokens[t % SIZE] order is
    * queue order. */
   uint64_t tail;
   /* Tokens that outran queue space, kept in order and flushed before
    * every new publish. */
   struct npt_evt_overflow_entry *ovf_head;
   struct npt_evt_overflow_entry *ovf_tail;
};

void
npt_completion_producer_init(struct npt_completion_producer *prod);

void
npt_completion_producer_fini(struct npt_completion_producer *prod);

/* Publish `token`, flushing the overflow FIFO first (in order), then
 * pushing into the ring or appending to the overflow tail.  Never
 * sleeps: overflow retries are the caller's job via npt_completion_flush.
 * Returns false only when the overflow entry could not be allocated. */
bool
npt_completion_publish(struct npt_completion_producer *prod,
                       struct npt_context_status_page *page,
                       uint64_t token);

/* Retry the overflow FIFO.  Returns true when anything was published.
 * Callers should run it from an existing periodic context (the ring
 * sync-queue poll cycle) when npt_completion_has_overflow is set. */
bool
npt_completion_flush(struct npt_completion_producer *prod,
                     struct npt_context_status_page *page);

/* Cheap unlocked hint that the overflow FIFO is non-empty.  A racy
 * read is fine: a false negative only delays the retry one poll. */
static inline bool
npt_completion_has_overflow(struct npt_completion_producer *prod)
{
   return prod->ovf_head != NULL;
}

/* After publish and proxy release: seq_cst fence, then request one
 * interrupt when a guest waiter exists and irq_pending flips 0 -> 1.
 * Returns true when the caller should signal the completion wake. */
bool
npt_completion_maybe_irq(struct npt_context_status_page *page);

#endif /* NPT_COMPLETION_H */
