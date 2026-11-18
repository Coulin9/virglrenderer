/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Renderer-level façade for the Neptune host runtime.
 */

#ifndef NPT_RENDERER_H
#define NPT_RENDERER_H

#include "config.h"

#include <stddef.h>
#include <stdint.h>

#include "virgl_resource.h"
#include "virglrenderer.h"

struct npt_d3d_library;
struct npt_work_context_config;

#define NPT_RENDERER_THREAD_SYNC (1u << 0)
#define NPT_RENDERER_ASYNC_FENCE_CB (1u << 1)

typedef void (*npt_renderer_retire_fence_callback_type)(uint32_t ctx_id,
                                                         uint32_t ring_idx,
                                                         uint64_t fence_id);

struct npt_renderer_callbacks {
   virgl_log_callback_type debug_logger;
   npt_renderer_retire_fence_callback_type retire_fence;
};

size_t
npt_get_capset(void *capset, uint32_t flags);

bool
npt_renderer_init(uint32_t flags, const struct npt_renderer_callbacks *cbs);

void
npt_renderer_fini(void);

/* Returns NULL if not initialized. */
struct npt_d3d_library *
npt_renderer_get_library(void);

/* Renderer API.  Manages an internal context table keyed by ctx_id. */
bool
npt_renderer_create_context(uint32_t ctx_id,
                            uint32_t ctx_flags,
                            uint32_t nlen,
                            const char *name);

void
npt_renderer_destroy_context(uint32_t ctx_id);

void
npt_renderer_set_work_context(uint32_t ctx_id,
                              const struct npt_work_context_config *config);

struct npt_context_status_page;
typedef void (*npt_status_wake_fn)(void *opaque);

/* Called once at INIT with the context status page mapping and a wake
 * that reaches the VMM's fence sync thread (render_wake_signal on the
 * worker side).  The page carries the completion queue, the lost flag
 * and irq_pending. */
void
npt_renderer_set_status_page(uint32_t ctx_id,
                             struct npt_context_status_page *page,
                             npt_status_wake_fn wake, void *wake_opaque);

bool
npt_renderer_submit_cmd(uint32_t ctx_id, void *cmd, uint32_t size);

bool
npt_renderer_submit_fence(uint32_t ctx_id,
                          uint32_t flags,
                          uint64_t ring_idx,
                          uint64_t fence_id);

bool
npt_renderer_create_resource(uint32_t ctx_id,
                             uint32_t res_id,
                             uint64_t blob_id,
                             uint64_t blob_size,
                             uint32_t blob_flags,
                             enum virgl_resource_fd_type *out_fd_type,
                             int *out_res_fd,
                             uint32_t *out_map_info,
                             uint32_t *out_export_format);

bool
npt_renderer_import_resource(uint32_t ctx_id,
                             uint32_t res_id,
                             enum virgl_resource_fd_type fd_type,
                             int fd,
                             uint64_t size);

void
npt_renderer_destroy_resource(uint32_t ctx_id, uint32_t res_id);

#endif /* NPT_RENDERER_H */
