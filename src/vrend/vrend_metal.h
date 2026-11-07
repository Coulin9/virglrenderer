/*
 * Copyright 2025 Turing Software, LLC
 * SPDX-License-Identifier: MIT
 */
#ifndef VIRGL_METAL_H
#define VIRGL_METAL_H

#include <stddef.h>

#include "virglrenderer.h"

typedef void *MTLDevice_id;
typedef void *MTLTexture_id;

struct vrend_metal_texture_description {
   unsigned width;
   unsigned height;
   unsigned stride;
   unsigned offset;
   unsigned bind;
   unsigned usage;
   uint32_t format;
};

bool virgl_metal_create_texture(MTLDevice_id device,
                                const struct vrend_metal_texture_description *desc,
                                MTLTexture_id *tex);

/*
 * The system default Metal device, cached for the lifetime of the process.
 * This lets code that never initialized vrend/EGL (such as a render-server
 * VMM) still wrap a shared-memory scanout as a MTLTexture.
 */
MTLDevice_id virgl_metal_default_device(void);

/*
 * Reconstruct a MTLTexture from a shared-memory FD. In this tree, cross-process
 * Metal resources (created by either Neptune or Venus contexts) are backed by a
 * mmap()-able shared-memory segment (VIRGL_RESOURCE_FD_SHM) rather than a raw
 * MTLTexture/MTLHeap handle. The FD is wrapped as an MTLBuffer and a linear
 * texture is created on top of it so it can be handed to ANGLE/Metal.
 *
 * With `require_aligned_stride`, a description whose row stride already
 * violates the device's linear-texture alignment is rejected instead of being
 * silently rounded up: rounding changes the row layout and corrupts the image.
 * Scanout callers pass true; importers that hand over a layout they cannot
 * change keep the historical rounding behaviour.
 */
bool virgl_metal_create_texture_from_shm(MTLDevice_id device,
                                         int fd,
                                         size_t size,
                                         const struct vrend_metal_texture_description *desc,
                                         bool require_aligned_stride,
                                         MTLTexture_id *tex);

MTLTexture_id virgl_metal_retain_texture(MTLTexture_id tex);

void virgl_metal_release_texture(MTLTexture_id tex);

#endif
