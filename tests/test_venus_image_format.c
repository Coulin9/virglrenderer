/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include "venus/vkr_image_format.h"

#define CHECK(condition) do { \
   if (!(condition)) { \
      fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
      return 1; \
   } \
} while (0)

int main(void)
{
   const uint32_t rgba[] = {
      VIRGL_FORMAT_R8G8B8A8_UNORM, VIRGL_FORMAT_R8G8B8A8_SRGB,
      VIRGL_FORMAT_R8G8B8X8_UNORM, VIRGL_FORMAT_R8G8B8X8_SRGB,
   };
   const uint32_t bgra[] = {
      VIRGL_FORMAT_B8G8R8A8_UNORM, VIRGL_FORMAT_B8G8R8A8_SRGB,
      VIRGL_FORMAT_B8G8R8X8_UNORM, VIRGL_FORMAT_B8G8R8X8_SRGB,
   };
   for (unsigned i = 0; i < 4; i++) {
      CHECK(vkr_image_format_needs_rb_swap(rgba[i], VK_FORMAT_B8G8R8A8_UNORM));
      CHECK(vkr_image_format_needs_rb_swap(rgba[i], VK_FORMAT_B8G8R8A8_SRGB));
      CHECK(!vkr_image_format_needs_rb_swap(rgba[i], VK_FORMAT_R8G8B8A8_UNORM));
      CHECK(vkr_image_format_needs_rb_swap(bgra[i], VK_FORMAT_R8G8B8A8_UNORM));
      CHECK(!vkr_image_format_needs_rb_swap(bgra[i], VK_FORMAT_B8G8R8A8_UNORM));
   }
   CHECK(!vkr_image_format_needs_rb_swap(0, VK_FORMAT_B8G8R8A8_UNORM));
   CHECK(!vkr_image_format_needs_rb_swap(VIRGL_FORMAT_R16G16B16A16_FLOAT,
                                         VK_FORMAT_R16G16B16A16_SFLOAT));
   VkComponentMapping identity = {0};
   vkr_image_format_swap_rb(&identity);
   CHECK(identity.r == VK_COMPONENT_SWIZZLE_B);
   CHECK(identity.g == VK_COMPONENT_SWIZZLE_G);
   CHECK(identity.b == VK_COMPONENT_SWIZZLE_R);
   CHECK(identity.a == VK_COMPONENT_SWIZZLE_A);
   VkComponentMapping custom = {
      VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_B,
      VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R,
   };
   vkr_image_format_swap_rb(&custom);
   CHECK(custom.r == VK_COMPONENT_SWIZZLE_ZERO);
   CHECK(custom.g == VK_COMPONENT_SWIZZLE_R);
   CHECK(custom.b == VK_COMPONENT_SWIZZLE_ONE);
   CHECK(custom.a == VK_COMPONENT_SWIZZLE_B);
   puts("PASS: imported image channel order and component mapping");
   return 0;
}
