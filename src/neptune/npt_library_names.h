/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef NPT_LIBRARY_NAMES_H
#define NPT_LIBRARY_NAMES_H

/*
 * Env-var names and default names of the host D3D backend libraries.
 * Shared by the backend loader, capset probe and render worker.
 *
 * On darwin all three slots name one backend umbrella, dlopened and
 * dlsym'd at runtime: libd3dmetal-native on the x86_64 slice,
 * libdxmt-native on the arm64 slice by default. The explicit Mullion
 * selection uses libmullion-native on arm64.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define NPT_D3D11_LIBRARY_ENV "NPT_D3D11_LIBRARY_PATH"
#define NPT_DXGI_LIBRARY_ENV  "NPT_DXGI_LIBRARY_PATH"
#define NPT_D3D12_LIBRARY_ENV "NPT_D3D12_LIBRARY_PATH"

#define NPT_MULLION_BACKEND_NAME "mullion"
#define NPT_MULLION_LIBRARY_NAME "libmullion-native.dylib"

static inline bool
npt_mullion_selected(void)
{
#ifdef __APPLE__
   const char *backend = getenv("NPT_BACKEND");
   return backend && !strcmp(backend, NPT_MULLION_BACKEND_NAME);
#else
   return false;
#endif
}

/* AppleDeck per-context backend routing: a tagged guest context names itself
 * "ad:d:<exe>" (d3dmetal) or "ad:m:<exe>" (mullion). The tag is an allowlist
 * keyword only -- never a filesystem path. The guest-side writer lives in
 * virtio-win-mesa's unixlib and must stay in sync with these spellings. */
#define NPT_APPLEDECK_TAG_LEN 5

static inline const char *
npt_appledeck_ctx_backend(const char *ctx_name)
{
   if (!ctx_name)
      return NULL;
   if (!strncmp(ctx_name, "ad:d:", NPT_APPLEDECK_TAG_LEN))
      return "d3dmetal";
   if (!strncmp(ctx_name, "ad:m:", NPT_APPLEDECK_TAG_LEN))
      return "mullion";
   return NULL;
}

/* Host-provided allowlist variable holding the validated adapter path for a
 * tagged backend. Set by the launcher only when that backend passed startup
 * validation, so a guest tag can never steer a worker to an untrusted path. */
static inline const char *
npt_appledeck_adapter_env(const char *backend)
{
   if (!strcmp(backend, "d3dmetal"))
      return "NPT_APPLEDECK_D3DMETAL_ADAPTER";
   if (!strcmp(backend, "mullion"))
      return "NPT_APPLEDECK_MULLION_ADAPTER";
   return NULL;
}

#if defined(__APPLE__) && defined(__aarch64__)
#define NPT_D3D11_LIBRARY_DEFAULT "libdxmt-native.dylib"
#define NPT_DXGI_LIBRARY_DEFAULT  "libdxmt-native.dylib"
#define NPT_D3D12_LIBRARY_DEFAULT "libdxmt-native.dylib"
#elif defined(__APPLE__)
#define NPT_D3D11_LIBRARY_DEFAULT "libd3dmetal-native.dylib"
#define NPT_DXGI_LIBRARY_DEFAULT  "libd3dmetal-native.dylib"
#define NPT_D3D12_LIBRARY_DEFAULT "libd3dmetal-native.dylib"
#else
#define NPT_D3D11_LIBRARY_DEFAULT "libd3d11.so"
#define NPT_DXGI_LIBRARY_DEFAULT  "libdxgi.so"
#define NPT_D3D12_LIBRARY_DEFAULT "libvkd3d-proton-d3d12.so"
#endif

#endif /* NPT_LIBRARY_NAMES_H */
