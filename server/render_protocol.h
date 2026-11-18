/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef RENDER_PROTOCOL_H
#define RENDER_PROTOCOL_H

#include <stdint.h>

#include "virgl_resource.h"
#include "neptune/npt_work_context.h"

/* this covers the command line options and the socket type */
#define RENDER_SERVER_VERSION 0

/* Internal socket layouts, checked before any context is created. */
#define RENDER_PROTOCOL_VERSION 1

/* The protocol itself is internal to virglrenderer.  There is no backward
 * compatibility to be kept.
 */

/* client ops, which are handled by the server process */
enum render_client_op {
   RENDER_CLIENT_OP_NOP = 0,
   RENDER_CLIENT_OP_INIT,
   RENDER_CLIENT_OP_RESET,
   RENDER_CLIENT_OP_CREATE_CONTEXT,
   RENDER_CLIENT_OP_DESTROY_CONTEXT,

   RENDER_CLIENT_OP_COUNT,
};

/* context ops, which are handled by workers (subprocesses or threads) created
 * by the server process
 */
enum render_context_op {
   RENDER_CONTEXT_OP_NOP = 0,
   RENDER_CONTEXT_OP_INIT,
   RENDER_CONTEXT_OP_CREATE_RESOURCE,
   RENDER_CONTEXT_OP_IMPORT_RESOURCE,
   RENDER_CONTEXT_OP_DESTROY_RESOURCE,
   RENDER_CONTEXT_OP_SUBMIT_CMD,
   RENDER_CONTEXT_OP_SUBMIT_FENCE,

   RENDER_CONTEXT_OP_COUNT,
};

struct render_client_op_header {
   enum render_client_op op;
};

struct render_client_op_nop_request {
   struct render_client_op_header header;
};

/* Initialize virglrenderer.
 *
 * This roughly corresponds to virgl_renderer_init.
 */
struct render_client_op_init_request {
   struct render_client_op_header header;
   uint32_t flags; /* VIRGL_RENDERER_USE_* and others */
   uint32_t protocol_version;
};

/* Remove all contexts.
 *
 * This roughly corresponds to virgl_renderer_reset.
 */
struct render_client_op_reset_request {
   struct render_client_op_header header;
};

/* Create a context, which will be serviced by a worker.
 *
 * See also the comment before main() for the process model.
 *
 * This roughly corresponds to virgl_renderer_context_create_with_flags.
 */
struct render_client_op_create_context_request {
   struct render_client_op_header header;
   uint32_t ctx_id;
   /* the context's capset, so the server can pick the worker's backend (and,
    * where a backend is confined to one architecture, the worker's slice of a
    * universal binary) before the worker is spawned */
   uint32_t capset_id;
   char ctx_name[32];
};

struct render_client_op_create_context_reply {
   bool ok;
   uint32_t worker_pid; /* actual spawned worker, for direct XPC peer validation */
   /* followed by 1 socket fd if ok */
};

/* Destroy a context, including the worker.
 *
 * This roughly corresponds to virgl_renderer_context_destroy.
 */
struct render_client_op_destroy_context_request {
   struct render_client_op_header header;
   uint32_t ctx_id;
};

union render_client_op_request {
   struct render_client_op_header header;
   struct render_client_op_nop_request nop;
   struct render_client_op_init_request init;
   struct render_client_op_reset_request reset;
   struct render_client_op_create_context_request create_context;
   struct render_client_op_destroy_context_request destroy_context;
};

struct render_context_op_header {
   enum render_context_op op;
};

struct render_context_op_nop_request {
   struct render_context_op_header header;
};

/* Initialize the context.
 *
 * The shmem is required and holds the shared submit ring region described
 * by server/render_ring.h: a render_ring_control block (timeline seqnos,
 * ring cursors, consumer states, flags and stats) followed by the main
 * byte ring and, when oob_ring_size is non-zero, the out-of-band fence
 * slot ring.  When ring_flags has RENDER_RING_FLAG_LATENCY_TRACE, a
 * render_ring_lat_area follows the rings at lat_ring_offset.
 *
 * ring_flags carries RENDER_RING_FLAG_* bits; in particular
 * RENDER_RING_FLAG_ALWAYS_NOTIFY asks the worker to signal the completion
 * wake fd on every timeline update (non-ASYNC_FENCE_CB clients).
 *
 * This roughly corresponds to virgl_renderer_context_create_with_flags.
 */
struct render_context_op_init_request {
   struct render_context_op_header header;
   uint32_t flags; /* VIRGL_RENDERER_CONTEXT_FLAG_*/
   uint32_t ring_flags; /* RENDER_RING_FLAG_* */
   size_t shmem_size;
   uint32_t main_ring_offset;
   uint32_t main_ring_size;
   uint32_t oob_ring_offset;
   uint32_t oob_ring_size; /* 0 = no out-of-band fence ring */
   uint32_t lat_ring_offset; /* 0 = no latency trace area */
   uint32_t lat_ring_size;
   struct npt_work_context_config work_context;
   /* On macOS: followed by the shmem fd only; the wake semaphores are
    * handed over as registered ports at spawn time and there is no
    * death pipe.  Elsewhere: followed by, in order, shmem fd,
    * completion-wake write end, main-ring wake read end, death-pipe
    * write end, and (only when oob_ring_size != 0) the oob-ring wake
    * read end. */
};

/* Export a blob resource from the context
 *
 * This roughly corresponds to:
 * - virgl_renderer_resource_create_blob
 * - virgl_renderer_resource_get_map_info
 * - virgl_renderer_resource_export_blob
 * - virgl_renderer_ctx_attach_resource
 */
struct render_context_op_create_resource_request {
   struct render_context_op_header header;
   uint32_t res_id;
   uint64_t blob_id;
   uint64_t blob_size;
   uint32_t blob_flags; /* VIRGL_RENDERER_BLOB_FLAG_* */
};

struct render_context_op_create_resource_reply {
   enum virgl_resource_fd_type fd_type;
   uint32_t map_info; /* VIRGL_RENDERER_MAP_* */
   /* vulkan_info is set if the fd_type is VIRGL_RESOURCE_FD_OPAQUE */
   struct virgl_resource_vulkan_info vulkan_info;
   /* enum virgl_formats the exporting context created the texture with,
    * or 0 if unknown.  The client cannot derive this: the blob crosses
    * to it as a bare fd, and the guest's own idea of the format is a
    * depth/bpp guess with no channel order in it. */
   uint32_t export_format;
   /* followed by 1 fd if not VIRGL_RESOURCE_FD_INVALID */
};

/* Import a blob resource to the context
 *
 * This roughly corresponds to:
 * - virgl_renderer_resource_import_blob
 * - virgl_renderer_ctx_attach_resource
 */
struct render_context_op_import_resource_request {
   struct render_context_op_header header;
   uint32_t res_id;
   enum virgl_resource_fd_type fd_type;
   uint64_t size;
   uint32_t export_format;
   /* followed by 1 fd */
};

/* Free a blob resource from the context
 *
 * This roughly corresponds to:
 * - virgl_renderer_resource_unref
 */
struct render_context_op_destroy_resource_request {
   struct render_context_op_header header;
   uint32_t res_id;
};

/* Submit a small command stream to the context.
 *
 * The size limit depends on the socket type.  Currently, SOCK_SEQPACKET is
 * used and the size limit is best treated as one page.
 *
 * This roughly corresponds to virgl_renderer_submit_cmd.
 */
struct render_context_op_submit_cmd_request {
   struct render_context_op_header header;
   uint32_t size;
   char cmd[256];
   /* if size > sizeof(cmd), followed by (size - sizeof(cmd)) bytes in another
    * message; size still must be small
    */
};

/* Submit a fence to the context.
 *
 * This submits a fence to the specified ring.  When the fence signals, the
 * current sequence number of the ring in the shmem is updated.
 *
 * This roughly corresponds to virgl_renderer_context_create_fence.
 *
 * No reply; retirement rides the shmem timeline and the fence eventfd.
 */
struct render_context_op_submit_fence_request {
   struct render_context_op_header header;
   uint32_t flags; /* VIRGL_RENDERER_FENCE_FLAG_* */
   /* TODO fix virgl_renderer_context_create_fence to use ring_index */
   uint32_t ring_index;
   uint32_t seqno;
};

union render_context_op_request {
   struct render_context_op_header header;
   struct render_context_op_nop_request nop;
   struct render_context_op_init_request init;
   struct render_context_op_create_resource_request create_resource;
   struct render_context_op_import_resource_request import_resource;
   struct render_context_op_destroy_resource_request destroy_resource;
   struct render_context_op_submit_cmd_request submit_cmd;
   struct render_context_op_submit_fence_request submit_fence;
};

#ifdef __APPLE__
/**
 * When we do not have SOCK_SEQPACKET support, we need to manage framing.
 * This will be sent as the header of each packet when necessary.
 */
struct render_context_socket_header {
   union {
      uint32_t length;
      uint8_t b[sizeof(uint32_t)];
   };
};
#endif /* __APPLE__ */

#endif /* RENDER_PROTOCOL_H */
