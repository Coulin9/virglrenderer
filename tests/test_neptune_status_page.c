/*
 * Copyright 2026
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <sys/mman.h>

#include "proxy/proxy_status_page.h"

static int failures;

#define CHECK(cond, what)                                                    \
   do {                                                                      \
      if (!(cond)) {                                                         \
         fprintf(stderr, "test_neptune_status_page: FAIL %s\n", what);       \
         failures++;                                                         \
      }                                                                      \
   } while (0)

int
main(void)
{
   CHECK(sizeof(struct npt_context_status_page) == 24, "status page size");

   struct npt_context_status_page *page =
      mmap(NULL, NPT_CONTEXT_STATUS_PAGE_SIZE, PROT_READ | PROT_WRITE,
           MAP_SHARED | MAP_ANON, -1, 0);
   if (page == MAP_FAILED) {
      fprintf(stderr, "test_neptune_status_page: mmap failed\n");
      return 1;
   }

   proxy_status_page_init(page);
   CHECK(page->magic == NPT_CONTEXT_STATUS_MAGIC, "magic");
   CHECK(page->version == NPT_CONTEXT_STATUS_VERSION, "version");
   CHECK(atomic_load_explicit(&page->lost, memory_order_acquire) == 0,
         "page starts alive");

   proxy_status_page_mark_lost(page, NPT_CONTEXT_STATUS_LOST_WORKER_EXIT,
                               12345);
   CHECK(atomic_load_explicit(&page->lost, memory_order_acquire) ==
            NPT_CONTEXT_STATUS_LOST_WORKER_EXIT,
         "lost reason visible");
   CHECK(atomic_load_explicit(&page->lost_ns, memory_order_acquire) == 12345,
         "lost_ns visible");

   munmap(page, NPT_CONTEXT_STATUS_PAGE_SIZE);

   if (failures) {
      fprintf(stderr, "test_neptune_status_page: %d check(s) failed\n", failures);
      return 1;
   }
   printf("test_neptune_status_page: all checks passed\n");
   return 0;
}
