/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "proxy_client.h"

#include <unistd.h>

#include "server/render_protocol.h"

#ifdef __APPLE__

#include "proxy_worker.h"

/* On macOS there is no render server: INIT, RESET, CREATE_CONTEXT and
 * DESTROY_CONTEXT are all handled locally, and the proxy spawns each
 * context worker itself with the ring wake semaphores as registered
 * ports. */

bool
proxy_client_destroy_context(struct proxy_client *client, uint32_t ctx_id)
{
   struct proxy_worker *worker = proxy_worker_find(ctx_id);
   if (worker)
      proxy_worker_destroy(worker);

   (void)client;
   return true;
}

bool
proxy_client_create_context(struct proxy_client *client,
                            uint32_t ctx_id,
                            uint32_t capset_id,
                            size_t ctx_name_len,
                            const char *ctx_name,
                            int *out_ctx_fd,
                            uint32_t *out_worker_pid,
                            struct proxy_worker *worker)
{
   char name[32];
   const size_t len = MIN2(ctx_name_len, sizeof(name) - 1);
   memcpy(name, ctx_name, len);
   name[len] = 0;

   if (!proxy_worker_spawn(ctx_id, capset_id, name, client->init_flags, worker))
      return false;

   *out_ctx_fd = worker->ctx_fd;
   *out_worker_pid = (uint32_t)worker->pid;
   return true;
}

bool
proxy_client_reset(struct proxy_client *client)
{
   proxy_worker_kill_all();
   (void)client;
   return true;
}

void
proxy_client_destroy(struct proxy_client *client)
{
   free(client);
}

struct proxy_client *
proxy_client_create(struct proxy_server *srv, uint32_t flags)
{
   (void)srv;

   struct proxy_client *client = calloc(1, sizeof(*client));
   if (!client)
      return NULL;

   client->init_flags = flags;
   return client;
}

#else /* !__APPLE__ */

#include "proxy_server.h"

bool
proxy_client_destroy_context(struct proxy_client *client, uint32_t ctx_id)
{
   const struct render_client_op_destroy_context_request req = {
      .header.op = RENDER_CLIENT_OP_DESTROY_CONTEXT,
      .ctx_id = ctx_id,
   };

   return proxy_socket_send_request(&client->socket, &req, sizeof(req));
}

bool
proxy_client_create_context(struct proxy_client *client,
                            uint32_t ctx_id,
                            uint32_t capset_id,
                            size_t ctx_name_len,
                            const char *ctx_name,
                            int *out_ctx_fd,
                            uint32_t *out_worker_pid,
                            UNUSED struct proxy_worker *worker)
{
   struct render_client_op_create_context_request req = {
      .header.op = RENDER_CLIENT_OP_CREATE_CONTEXT,
      .ctx_id = ctx_id,
      .capset_id = capset_id,
   };

   const size_t len = MIN2(ctx_name_len, sizeof(req.ctx_name) - 1);
   memcpy(req.ctx_name, ctx_name, len);

   if (!proxy_socket_send_request(&client->socket, &req, sizeof(req)))
      return false;

   struct render_client_op_create_context_reply reply;
   int fd_count;
   int ctx_fd;
   if (!proxy_socket_receive_reply_with_fds(&client->socket, &reply, sizeof(reply),
                                            &ctx_fd, 1, &fd_count))
      return false;

   if (reply.ok != fd_count) {
      if (fd_count)
         close(ctx_fd);
      return false;
   } else if (!reply.ok) {
      return false;
   }

   if (!proxy_socket_is_valid(ctx_fd)) {
      close(ctx_fd);
      return false;
   }

   *out_ctx_fd = ctx_fd;
   *out_worker_pid = reply.worker_pid;
   return true;
}

bool
proxy_client_reset(struct proxy_client *client)
{
   const struct render_client_op_reset_request req = {
      .header.op = RENDER_CLIENT_OP_RESET,
   };
   return proxy_socket_send_request(&client->socket, &req, sizeof(req));
}

void
proxy_client_destroy(struct proxy_client *client)
{
   proxy_socket_fini(&client->socket);
   free(client);
}

static bool
proxy_client_init(struct proxy_client *client, uint32_t flags)
{
   const struct render_client_op_init_request req = {
      .header.op = RENDER_CLIENT_OP_INIT,
      .flags = flags,
      .protocol_version = RENDER_PROTOCOL_VERSION,
   };
   return proxy_socket_send_request(&client->socket, &req, sizeof(req));
}

struct proxy_client *
proxy_client_create(struct proxy_server *srv, uint32_t flags)
{
   struct proxy_client *client = calloc(1, sizeof(*client));
   if (!client)
      return NULL;

   const int client_fd = proxy_server_connect(srv);
   if (client_fd < 0) {
      free(client);
      return NULL;
   }

   proxy_socket_init(&client->socket, client_fd);

   if (!proxy_client_init(client, flags)) {
      proxy_socket_fini(&client->socket);
      free(client);
      return NULL;
   }

   return client;
}

#endif /* __APPLE__ */
