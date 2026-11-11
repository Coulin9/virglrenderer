/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_descriptor_set.h"

#include "vkr_descriptor_set_gen.h"
#include "vkr_image.h"

void
vkr_descriptor_set_replace_writes(uint32_t count, const VkWriteDescriptorSet *writes)
{
   for (uint32_t i = 0; writes && i < count; i++) {
      VkWriteDescriptorSet *write = (VkWriteDescriptorSet *)&writes[i];
      if (write->descriptorType != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
          write->descriptorType != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
         vn_replace_VkWriteDescriptorSet_handle(write);
         continue;
      }

      VkDescriptorImageInfo *images = (VkDescriptorImageInfo *)write->pImageInfo;
      write->pImageInfo = NULL;
      vn_replace_VkWriteDescriptorSet_handle(write);
      write->pImageInfo = images;
      for (uint32_t j = 0; images && j < write->descriptorCount; j++) {
         struct vkr_image_view *view = vkr_image_view_from_handle(images[j].imageView);
         vn_replace_VkSampler_handle(&images[j].sampler);
         images[j].imageView = view
            ? (view->sampled_view ? view->sampled_view : view->base.handle.image_view)
            : VK_NULL_HANDLE;
      }
   }
}

static void
vkr_dispatch_vkGetDescriptorSetLayoutSupport(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetDescriptorSetLayoutSupport *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetDescriptorSetLayoutSupport_args_handle(args);
   vk->GetDescriptorSetLayoutSupport(args->device, args->pCreateInfo, args->pSupport);
}

static void
vkr_dispatch_vkCreateDescriptorSetLayout(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkCreateDescriptorSetLayout *args)
{
   vkr_descriptor_set_layout_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkDestroyDescriptorSetLayout(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkDestroyDescriptorSetLayout *args)
{
   vkr_descriptor_set_layout_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkCreateDescriptorPool(struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkCreateDescriptorPool *args)
{
   struct vkr_descriptor_pool *pool =
      vkr_descriptor_pool_create_and_add(dispatch->data, args);
   if (!pool)
      return;

   pool->flags = args->pCreateInfo->flags;

   list_inithead(&pool->descriptor_sets);
}

static void
vkr_dispatch_vkDestroyDescriptorPool(struct vn_dispatch_context *dispatch,
                                     struct vn_command_vkDestroyDescriptorPool *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_descriptor_pool *pool =
      vkr_descriptor_pool_from_handle(args->descriptorPool);

   if (!pool)
      return;

   vkr_descriptor_pool_release(ctx, pool);
   vkr_descriptor_pool_destroy_and_remove(ctx, args);
}

static void
vkr_dispatch_vkResetDescriptorPool(struct vn_dispatch_context *dispatch,
                                   struct vn_command_vkResetDescriptorPool *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   struct vkr_context *ctx = dispatch->data;

   struct vkr_descriptor_pool *pool =
      vkr_descriptor_pool_from_handle(args->descriptorPool);
   if (!pool) {
      vkr_context_set_fatal(ctx);
      return;
   }

   vn_replace_vkResetDescriptorPool_args_handle(args);
   args->ret = vk->ResetDescriptorPool(args->device, args->descriptorPool, args->flags);

   vkr_descriptor_pool_release(ctx, pool);
   list_inithead(&pool->descriptor_sets);
}

static void
vkr_dispatch_vkAllocateDescriptorSets(struct vn_dispatch_context *dispatch,
                                      struct vn_command_vkAllocateDescriptorSets *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vkr_descriptor_pool *pool =
      vkr_descriptor_pool_from_handle(args->pAllocateInfo->descriptorPool);
   struct object_array arr;
   VkResult result;

   if (!pool) {
      vkr_context_set_fatal(ctx);
      return;
   }

   result = vkr_descriptor_set_create_array(ctx, args, &arr);
   if (result != VK_SUCCESS) {
      if (!(pool->flags & VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT))
         vkr_log("Warning: vkAllocateDescriptorSets failed(%u)", result);
      return;
   }

   vkr_descriptor_set_add_array(ctx, dev, pool, &arr);
}

static void
vkr_dispatch_vkFreeDescriptorSets(struct vn_dispatch_context *dispatch,
                                  struct vn_command_vkFreeDescriptorSets *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct list_head free_list;

   /* args->pDescriptorSets is marked noautovalidity="true" */
   if (args->descriptorSetCount && !args->pDescriptorSets) {
      vkr_context_set_fatal(ctx);
      return;
   }

   vkr_descriptor_set_destroy_driver_handles(ctx, args, &free_list);
   vkr_context_remove_objects(ctx, &free_list);

   args->ret = VK_SUCCESS;
}

static void
vkr_dispatch_vkUpdateDescriptorSets(UNUSED struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkUpdateDescriptorSets *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_VkDevice_handle(&args->device);
   vkr_descriptor_set_replace_writes(args->descriptorWriteCount, args->pDescriptorWrites);
   for (uint32_t i = 0; args->pDescriptorCopies && i < args->descriptorCopyCount; i++)
      vn_replace_VkCopyDescriptorSet_handle((VkCopyDescriptorSet *)&args->pDescriptorCopies[i]);
   vk->UpdateDescriptorSets(args->device, args->descriptorWriteCount,
                            args->pDescriptorWrites, args->descriptorCopyCount,
                            args->pDescriptorCopies);
}

static void
vkr_dispatch_vkCreateDescriptorUpdateTemplate(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkCreateDescriptorUpdateTemplate *args)
{
   vkr_descriptor_update_template_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkDestroyDescriptorUpdateTemplate(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkDestroyDescriptorUpdateTemplate *args)
{
   vkr_descriptor_update_template_destroy_and_remove(dispatch->data, args);
}

void
vkr_context_init_descriptor_set_layout_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkGetDescriptorSetLayoutSupport =
      vkr_dispatch_vkGetDescriptorSetLayoutSupport;
   dispatch->dispatch_vkCreateDescriptorSetLayout =
      vkr_dispatch_vkCreateDescriptorSetLayout;
   dispatch->dispatch_vkDestroyDescriptorSetLayout =
      vkr_dispatch_vkDestroyDescriptorSetLayout;
}

void
vkr_context_init_descriptor_pool_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateDescriptorPool = vkr_dispatch_vkCreateDescriptorPool;
   dispatch->dispatch_vkDestroyDescriptorPool = vkr_dispatch_vkDestroyDescriptorPool;
   dispatch->dispatch_vkResetDescriptorPool = vkr_dispatch_vkResetDescriptorPool;
}

void
vkr_context_init_descriptor_set_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkAllocateDescriptorSets = vkr_dispatch_vkAllocateDescriptorSets;
   dispatch->dispatch_vkFreeDescriptorSets = vkr_dispatch_vkFreeDescriptorSets;
   dispatch->dispatch_vkUpdateDescriptorSets = vkr_dispatch_vkUpdateDescriptorSets;
}

void
vkr_context_init_descriptor_update_template_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateDescriptorUpdateTemplate =
      vkr_dispatch_vkCreateDescriptorUpdateTemplate;
   dispatch->dispatch_vkDestroyDescriptorUpdateTemplate =
      vkr_dispatch_vkDestroyDescriptorUpdateTemplate;
   dispatch->dispatch_vkUpdateDescriptorSetWithTemplate = NULL;
}
