/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef PROXY_CLIENT_H
#define PROXY_CLIENT_H

#include "proxy_common.h"

#include "proxy_worker.h"

struct proxy_client {
   struct proxy_socket socket;

#ifdef __APPLE__
   /* the renderer init flags, stored locally instead of sent to a
    * server: on macOS the proxy spawns each context worker itself */
   uint32_t init_flags;
#endif
};

struct proxy_client *
proxy_client_create(struct proxy_server *srv, uint32_t flags);

void
proxy_client_destroy(struct proxy_client *client);

bool
proxy_client_reset(struct proxy_client *client);

/* On macOS, worker is a caller-owned struct proxy_worker to be filled by
 * a direct spawn (it carries the ctx_id, semaphores, and the monitor
 * callback set by the caller before this call); ignored elsewhere. */
bool
proxy_client_create_context(struct proxy_client *client,
                            uint32_t ctx_id,
                            uint32_t capset_id,
                            size_t ctx_name_len,
                            const char *ctx_name,
                            int *out_ctx_fd,
                            uint32_t *out_worker_pid,
                            struct proxy_worker *worker);

bool
proxy_client_destroy_context(struct proxy_client *client, uint32_t ctx_id);

#endif /* PROXY_CLIENT_H */
