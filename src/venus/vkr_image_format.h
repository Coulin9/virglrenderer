/* SPDX-License-Identifier: MIT */
#ifndef VKR_IMAGE_FORMAT_H
#define VKR_IMAGE_FORMAT_H

#include <stdbool.h>
#include "venus-protocol/vulkan.h"
#include "virgl_hw.h"

static inline bool
vkr_image_format_needs_rb_swap(uint32_t source, VkFormat view)
{
   switch (source) {
   case VIRGL_FORMAT_R8G8B8A8_UNORM:
   case VIRGL_FORMAT_R8G8B8A8_SRGB:
   case VIRGL_FORMAT_R8G8B8X8_UNORM:
   case VIRGL_FORMAT_R8G8B8X8_SRGB:
      return view == VK_FORMAT_B8G8R8A8_UNORM || view == VK_FORMAT_B8G8R8A8_SRGB;
   case VIRGL_FORMAT_B8G8R8A8_UNORM:
   case VIRGL_FORMAT_B8G8R8A8_SRGB:
   case VIRGL_FORMAT_B8G8R8X8_UNORM:
   case VIRGL_FORMAT_B8G8R8X8_SRGB:
      return view == VK_FORMAT_R8G8B8A8_UNORM || view == VK_FORMAT_R8G8B8A8_SRGB;
   default:
      return false;
   }
}

static inline VkComponentSwizzle
vkr_image_format_swap_component(VkComponentSwizzle component,
                                VkComponentSwizzle identity)
{
   if (component == VK_COMPONENT_SWIZZLE_IDENTITY)
      component = identity;
   if (component == VK_COMPONENT_SWIZZLE_R)
      return VK_COMPONENT_SWIZZLE_B;
   if (component == VK_COMPONENT_SWIZZLE_B)
      return VK_COMPONENT_SWIZZLE_R;
   return component;
}

static inline void
vkr_image_format_swap_rb(VkComponentMapping *components)
{
   components->r = vkr_image_format_swap_component(components->r, VK_COMPONENT_SWIZZLE_R);
   components->g = vkr_image_format_swap_component(components->g, VK_COMPONENT_SWIZZLE_G);
   components->b = vkr_image_format_swap_component(components->b, VK_COMPONENT_SWIZZLE_B);
   components->a = vkr_image_format_swap_component(components->a, VK_COMPONENT_SWIZZLE_A);
}

#endif
