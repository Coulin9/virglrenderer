/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "render_context.h"
#include "render_server.h"
#include "neptune/npt_work_context.h"

#if defined(__APPLE__) && defined(ENABLE_NEPTUNE)
#include <stdlib.h>
#include <string.h>

#include "neptune/npt_library_names.h"
#include "virglrenderer.h"

/* A guest-tagged context ("ad:d:<exe>" / "ad:m:<exe>") asks for a specific
 * validated backend adapter. Apply it in this worker only: the spawned worker
 * is a fresh process image, so setenv here can never leak into the server's
 * shared environ or affect contexts already running in other workers. */
static bool
appledeck_apply_ctx_backend(struct render_context_args *args)
{
   if (!(args->init_flags & VIRGL_RENDERER_NEPTUNE))
      return true;
   const char *backend = npt_appledeck_ctx_backend(args->ctx_name);
   if (!backend)
      return true;
   const char *env_name = npt_appledeck_adapter_env(backend);
   const char *adapter = env_name ? getenv(env_name) : NULL;
   if (!adapter || !*adapter) {
      render_log("Neptune worker %u: refusing tagged context: %s is not set",
                 args->ctx_id, env_name ? env_name : "?");
      return false;
   }
   setenv("NPT_BACKEND", backend, 1);
   setenv(NPT_D3D11_LIBRARY_ENV, adapter, 1);
   setenv(NPT_DXGI_LIBRARY_ENV, adapter, 1);
   setenv(NPT_D3D12_LIBRARY_ENV, adapter, 1);
   memmove(args->ctx_name, args->ctx_name + NPT_APPLEDECK_TAG_LEN,
           strlen(args->ctx_name + NPT_APPLEDECK_TAG_LEN) + 1);
   render_log("Neptune worker %u: appledeck backend=%s ctx=%s",
              args->ctx_id, backend, args->ctx_name);
   return true;
}
#endif

#if defined(__APPLE__) && defined(__x86_64__)
#include <mach/mach.h>
#include <mach/task_policy.h>

#include "virglrenderer.h"

static void
set_neptune_worker_latency(const struct render_context_args *args)
{
   if (!(args->init_flags & VIRGL_RENDERER_NEPTUNE))
      return;

   /* Apply the default latency policy in the D3DMetal worker before the
    * renderer starts its threads.  Policy errors are logged and context
    * startup continues with the kernel's current policy.
    */
   struct task_qos_policy policy = {
      .task_latency_qos_tier = LATENCY_QOS_TIER_0,
      .task_throughput_qos_tier = THROUGHPUT_QOS_TIER_UNSPECIFIED,
   };
   const kern_return_t ret = task_policy_set(
      mach_task_self(), TASK_BASE_LATENCY_QOS_POLICY,
      (task_policy_t)&policy, TASK_QOS_POLICY_COUNT);
   if (ret != KERN_SUCCESS) {
      render_log("Neptune worker %u: latency tier 0 failed: %s",
                 args->ctx_id, mach_error_string(ret));
      return;
   }

   mach_msg_type_number_t count = TASK_QOS_POLICY_COUNT;
   boolean_t get_default = FALSE;
   const kern_return_t query_ret = task_policy_get(
      mach_task_self(), TASK_BASE_QOS_POLICY,
      (task_policy_t)&policy, &count, &get_default);
   if (query_ret != KERN_SUCCESS) {
      render_log("Neptune worker %u: latency policy readback failed: %s",
                 args->ctx_id, mach_error_string(query_ret));
      return;
   }

   render_log("Neptune worker %u: latency tier 0 enabled, base=0x%x throughput=0x%x",
              args->ctx_id, policy.task_latency_qos_tier,
              policy.task_throughput_qos_tier);
}
#endif

/* The main process is the server process.  It enters render_server_main and
 * never returns except on fatal errors.
 *
 * The server process supports only one connection currently.  It creates a
 * render_client to manage the connection.  There is a client process at the
 * other end of the connection.  When the client process requests a new
 * context to be created, the server process creates a worker.  It also sets
 * up a socket pair, with one end owned by the worker and the other end sent
 * to and owned by the client process.
 *
 * When a worker is a thread, the thread enters render_context_main directly
 * from its start function.  In this case, render_context_main must be
 * thread-safe.
 *
 * When a worker is a process,
 * - On Linux, it's a subprocess forked from the server process. It returns
 *   from render_server_main and enters render_context_main.
 * - On macOS, the worker is posix_spawn'd with --worker-context-* args.
 *   render_server_main parses these, skips the server loop, and returns
 *   with ctx_args.valid set for render_context_main.
 */
int
main(int argc, char **argv)
{
   render_log_init();

#if defined(__APPLE__) && defined(ENABLE_NEPTUNE)
   if ((argc == 2 || argc == 3) && !strcmp(argv[1], "--work-context-service"))
      return npt_work_context_service_main(argc == 3 ? argv[2] : "org.appledeck.render-context");
#endif

   struct render_context_args ctx_args;
   bool ok = render_server_main(argc, argv, &ctx_args);

   /* this is a subprocess */
   if (ok && ctx_args.valid) {
#if defined(__APPLE__) && defined(ENABLE_NEPTUNE)
      ok = appledeck_apply_ctx_backend(&ctx_args);
#endif
   }
   if (ok && ctx_args.valid) {
#if defined(__APPLE__) && defined(__x86_64__)
      set_neptune_worker_latency(&ctx_args);
#endif
      ok = render_context_main(&ctx_args);
   }

   return ok ? 0 : -1;
}
