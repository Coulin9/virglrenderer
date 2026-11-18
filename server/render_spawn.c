/*
 * Copyright 2026
 * SPDX-License-Identifier: MIT
 */

#include "render_spawn.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* shared between the render server binary and libvirglrenderer, which
 * use different loggers; keep a private stderr logger */
static void
render_spawn_log(const char *fmt, ...)
{
   va_list va;
   va_start(va, fmt);
   fprintf(stderr, "render_spawn: ");
   vfprintf(stderr, fmt, va);
   fprintf(stderr, "\n");
   va_end(va);
}

#include "virtgpu_drm.h"
#include "virglrenderer.h"

void
render_init_context_args(struct render_context_args *ctx_args,
                         uint32_t init_flags,
                         uint32_t capset_id,
                         uint32_t ctx_id,
                         const char *ctx_name,
                         size_t ctx_name_len,
                         int ctx_fd)
{
   /* A worker services a single context, so it only needs that context's
    * backend.  Scoping the flags keeps a worker from bringing up a backend it
    * will never use -- e.g. an x86_64 neptune worker must not also initialize
    * venus, which render_state_init treats as fatal if it fails. */
   const uint32_t backends = VIRGL_RENDERER_VENUS | VIRGL_RENDERER_NEPTUNE;
   uint32_t worker_flags = init_flags;
   switch (capset_id) {
   case VIRTGPU_DRM_CAPSET_VENUS:
      worker_flags = (init_flags & ~backends) | VIRGL_RENDERER_VENUS;
      break;
   case VIRTGPU_DRM_CAPSET_NEPTUNE:
      worker_flags = (init_flags & ~backends) | VIRGL_RENDERER_NEPTUNE;
      break;
   default:
      break;
   }

   *ctx_args = (struct render_context_args){
      .valid = true,
      .init_flags = worker_flags,
      .ctx_id = ctx_id,
      .ctx_fd = ctx_fd,
   };

   const size_t len = MIN2(ctx_name_len, sizeof(ctx_args->ctx_name) - 1);
   memcpy(ctx_args->ctx_name, ctx_name, len);
}

#ifdef __APPLE__

#include <crt_externs.h>
#include <mach/machine.h>
#include <spawn.h>

/* Not declared by the SDK <spawn.h>; exported by libSystem. */
extern int posix_spawnattr_set_registered_ports_np(posix_spawnattr_t *,
                                                   const mach_port_t *,
                                                   uint32_t);
#include <stdio.h>
#include <stdlib.h>

#include "neptune/npt_library_names.h"
#include "virglrenderer.h"

/* On macOS, fork() without exec() inherits stale XPC connections
 * (Mach port based), breaking Metal shader compilation in worker
 * subprocesses.  posix_spawn() atomically creates a fresh process
 * image without an intermediate forked state.
 *
 * POSIX_SPAWN_CLOEXEC_DEFAULT closes all fds by default in the child;
 * only ctx_fd is explicitly inherited via addinherit_np.
 *
 * The ring wake semaphores ride along as the child's task registered
 * ports (RENDER_WAKE_PORT_* order); the worker retrieves them with
 * mach_ports_lookup.  A NULL ports array spawns without registered
 * ports (the retired server-mediated path).
 */
pid_t
render_spawn_worker(const struct render_context_args *ctx_args,
                    const mach_port_t *ports)
{
   /* for devenv without installing server */
   char *const server_path = getenv("RENDER_SERVER_EXEC_PATH");
   const char *exec_path = server_path ? server_path : RENDER_SERVER_EXEC_PATH;

   char fd_str[16];
   char id_str[16];
   char flags_str[16];
   snprintf(fd_str, sizeof(fd_str), "%d", ctx_args->ctx_fd);
   snprintf(id_str, sizeof(id_str), "%u", ctx_args->ctx_id);
   snprintf(flags_str, sizeof(flags_str), "%u", ctx_args->init_flags);

   char *const argv[] = {
      (char *)exec_path,
      "--worker-context-fd",
      fd_str,
      "--worker-context-id",
      id_str,
      "--worker-context-init-flags",
      flags_str,
      "--worker-context-name",
      (char *)ctx_args->ctx_name,
      NULL,
   };

   posix_spawnattr_t attr;
   if (posix_spawnattr_init(&attr) != 0)
      return -1;
   posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);

   if (ports &&
       posix_spawnattr_set_registered_ports_np(&attr, ports,
                                               RENDER_WAKE_PORT_COUNT)) {
      render_spawn_log("failed to register worker ports");
      posix_spawnattr_destroy(&attr);
      return -1;
   }

   /* Venus, DXMT and Mullion use arm64. D3DMetal uses x86_64 under Rosetta.
    * CPU_TYPE_ANY lets a thin build fall back to its available slice. */
   cpu_type_t worker_arch = CPU_TYPE_ARM64;
   if (ctx_args->init_flags & VIRGL_RENDERER_NEPTUNE) {
      const char *backend = getenv("NPT_BACKEND");
      const bool use_dxmt = backend && !strcmp(backend, "dxmt");
      const bool native_backend = use_dxmt || npt_mullion_selected();
      if (backend && *backend && !native_backend && strcmp(backend, "d3dmetal"))
         render_spawn_log("unknown NPT_BACKEND '%s', using d3dmetal", backend);
      worker_arch = native_backend ? CPU_TYPE_ARM64 : CPU_TYPE_X86_64;
   }
   cpu_type_t cpu_pref[] = { worker_arch, CPU_TYPE_ANY };
   cpu_subtype_t cpu_subpref[] = { CPU_SUBTYPE_ANY, CPU_SUBTYPE_ANY };
   size_t ocount = 0;
   posix_spawnattr_setarchpref_np(&attr, 2, cpu_pref, cpu_subpref, &ocount);

   posix_spawn_file_actions_t file_actions;
   if (posix_spawn_file_actions_init(&file_actions) != 0) {
      posix_spawnattr_destroy(&attr);
      return -1;
   }
   posix_spawn_file_actions_addinherit_np(&file_actions, STDIN_FILENO);
   posix_spawn_file_actions_addinherit_np(&file_actions, STDOUT_FILENO);
   posix_spawn_file_actions_addinherit_np(&file_actions, STDERR_FILENO);
   posix_spawn_file_actions_addinherit_np(&file_actions, ctx_args->ctx_fd);

   pid_t pid;
   int ret = posix_spawn(&pid, exec_path, &file_actions, &attr, argv, *_NSGetEnviron());

   posix_spawn_file_actions_destroy(&file_actions);
   posix_spawnattr_destroy(&attr);

   if (ret != 0) {
      render_spawn_log("posix_spawn failed: %s", strerror(ret));
      return -1;
   }

   return pid;
}

#endif /* __APPLE__ */
