/*
 * Copyright 2026
 * SPDX-License-Identifier: MIT
 */

#ifndef PROXY_STATUS_PAGE_H
#define PROXY_STATUS_PAGE_H

#include <stdatomic.h>
#include <stdint.h>

#include "neptune/neptune-protocol/npt_protocol_transport.h"

static inline void
proxy_status_page_init(struct npt_context_status_page *page)
{
   page->magic = NPT_CONTEXT_STATUS_MAGIC;
   page->version = NPT_CONTEXT_STATUS_VERSION;
   atomic_store_explicit(&page->lost, 0, memory_order_relaxed);
   page->pad0 = 0;
   atomic_store_explicit(&page->lost_ns, 0, memory_order_relaxed);
}

static inline void
proxy_status_page_mark_lost(struct npt_context_status_page *page,
                            uint32_t reason, uint64_t now_ns)
{
   atomic_store_explicit(&page->lost_ns, now_ns, memory_order_relaxed);
   atomic_store_explicit(&page->lost, reason, memory_order_seq_cst);
}

#endif /* PROXY_STATUS_PAGE_H */
