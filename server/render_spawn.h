/*
 * Copyright 2026
 * SPDX-License-Identifier: MIT
 */

#ifndef RENDER_SPAWN_H
#define RENDER_SPAWN_H

#include "render_common.h"

#include "render_context.h"
#include "render_ring.h"

/* Fill a render_context_args for one context worker, scoping
 * VIRGL_RENDERER_VENUS / VIRGL_RENDERER_NEPTUNE to the context's capset
 * so a worker only initializes the backend it will use. */
void
render_init_context_args(struct render_context_args *ctx_args,
                         uint32_t init_flags,
                         uint32_t capset_id,
                         uint32_t ctx_id,
                         const char *ctx_name,
                         size_t ctx_name_len,
                         int ctx_fd);

#ifdef __APPLE__
/* posix_spawn the context worker.  When ports is non-NULL the ports are
 * registered with the child (posix_spawnattr_set_registered_ports_np)
 * in RENDER_WAKE_PORT_* order.  Returns the child pid or -1. */
pid_t
render_spawn_worker(const struct render_context_args *ctx_args,
                    const mach_port_t *ports);
#endif

#endif /* RENDER_SPAWN_H */
