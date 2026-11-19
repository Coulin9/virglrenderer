/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * DESC_BATCH: one wire command standing in for a run of per-call
 * descriptor writes the guest coalesced in its stage.  The entry values
 * are exactly what the individual commands carry, so the calls below
 * mirror the generated ID3D12Device dispatch: raw handle pass-through
 * to the backend vtable, routed through the dispatch overrides table
 * when one is installed.
 */

#include "npt_desc_batch.h"

#include <string.h>

#include "npt_com.h"
#include "npt_common.h"
#include "npt_cs.h"
#include "neptune-protocol/npt_protocol_host_id3d12device.h"

static void
npt_desc_batch_copy_simple(struct npt_dispatch_context *dispatch,
                           void *device, uint32_t heap_type,
                           const uint8_t *entries, uint32_t count)
{
   D3D12_CPU_DESCRIPTOR_HANDLE dst[NPT_DESC_BATCH_MAX_ENTRIES];
   D3D12_CPU_DESCRIPTOR_HANDLE src[NPT_DESC_BATCH_MAX_ENTRIES];
   UINT dst_num[NPT_DESC_BATCH_MAX_ENTRIES];
   UINT src_num[NPT_DESC_BATCH_MAX_ENTRIES];
   for (uint32_t i = 0; i < count; i++) {
      struct npt_desc_copy_entry e;
      memcpy(&e, entries + i * sizeof(e), sizeof(e));
      dst[i].ptr = e.dst;
      src[i].ptr = e.src;
      dst_num[i] = e.num;
      src_num[i] = e.num;
   }
   PFN_ID3D12Device_CopyDescriptors original = NPT_COM_VTBL_FUNC(
      PFN_ID3D12Device_CopyDescriptors, npt_com_vtable(device),
      NPT_VTBL_ID3D12Device_CopyDescriptors);
   if (dispatch->id3d12device_dispatch_overrides &&
       dispatch->id3d12device_dispatch_overrides->CopyDescriptors) {
      struct npt_command_ID3D12Device_CopyDescriptors args = {
         ._self = device,
         .NumDestDescriptorRanges = count,
         .pDestDescriptorRangeStarts = dst,
         .pDestDescriptorRangeSizes = dst_num,
         .NumSrcDescriptorRanges = count,
         .pSrcDescriptorRangeStarts = src,
         .pSrcDescriptorRangeSizes = src_num,
         .DescriptorHeapsType = (D3D12_DESCRIPTOR_HEAP_TYPE)heap_type,
      };
      dispatch->id3d12device_dispatch_overrides->CopyDescriptors(
         dispatch, &args, original);
   } else {
      original(device, count, dst, dst_num, count, src, src_num,
               (D3D12_DESCRIPTOR_HEAP_TYPE)heap_type);
   }
}

static void
npt_desc_batch_create_cbv(struct npt_dispatch_context *dispatch,
                          void *device, const uint8_t *entries,
                          uint32_t count)
{
   PFN_ID3D12Device_CreateConstantBufferView original = NPT_COM_VTBL_FUNC(
      PFN_ID3D12Device_CreateConstantBufferView, npt_com_vtable(device),
      NPT_VTBL_ID3D12Device_CreateConstantBufferView);
   for (uint32_t i = 0; i < count; i++) {
      struct npt_desc_cbv_entry e;
      memcpy(&e, entries + i * sizeof(e), sizeof(e));
      const D3D12_CONSTANT_BUFFER_VIEW_DESC desc = {
         .BufferLocation = e.buffer_location,
         .SizeInBytes = e.size_in_bytes,
      };
      const D3D12_CPU_DESCRIPTOR_HANDLE dst = { .ptr = e.dst };
      if (dispatch->id3d12device_dispatch_overrides &&
          dispatch->id3d12device_dispatch_overrides->CreateConstantBufferView) {
         struct npt_command_ID3D12Device_CreateConstantBufferView args = {
            ._self = device,
            .pDesc = e.has_desc ? &desc : NULL,
            .DestDescriptor = dst,
         };
         dispatch->id3d12device_dispatch_overrides
            ->CreateConstantBufferView(dispatch, &args, original);
      } else {
         original(device, e.has_desc ? &desc : NULL, dst);
      }
   }
}

void
npt_transport_desc_batch(UNUSED struct npt_context *ctx,
                         struct npt_dispatch_context *dispatch,
                         struct npt_cs_decoder *dec,
                         const struct npt_command_header *header)
{
   struct npt_cmd_desc_batch bh;
   bh.header = *header;
   npt_cs_decoder_read(dec, sizeof(bh) - sizeof(bh.header),
                       &bh.device_id, sizeof(bh) - sizeof(bh.header));
   if (npt_cs_decoder_get_fatal(dec))
      return;

   size_t entry_size;
   switch (bh.kind) {
   case NPT_DESC_BATCH_COPY_SIMPLE:
      entry_size = sizeof(struct npt_desc_copy_entry);
      if (bh.heap_type >= D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES)
         goto malformed;
      break;
   case NPT_DESC_BATCH_CREATE_CBV:
      entry_size = sizeof(struct npt_desc_cbv_entry);
      if (bh.heap_type != 0)
         goto malformed;
      break;
   default:
      goto malformed;
   }

   if (bh.count > NPT_DESC_BATCH_MAX_ENTRIES ||
       header->cmd_size != (uint64_t)sizeof(bh) + (uint64_t)bh.count * entry_size)
      goto malformed;

   /* The generated ID3D12Device dispatch drops reply-less calls on an
    * unknown object; DESC_BATCH never owes a reply, so a destroyed or
    * never-registered device drops rather than going fatal. */
   void *device = npt_cs_handle_lookup(dispatch, bh.device_id,
                                       NPT_OBJECT_TYPE_ID3D12DEVICE);
   if (!device) {
      (void)npt_cs_decoder_take_handle_miss(dec);
      npt_log("dropping DESC_BATCH on unregistered device 0x%016" PRIx64,
              (uint64_t)bh.device_id);
      return;
   }

   /* Entries read straight from the command stream; the decoder is
    * already bounded to cmd_size, so a short read is fatal there. */
   uint8_t entries[NPT_DESC_BATCH_MAX_ENTRIES *
                   sizeof(struct npt_desc_copy_entry)];
   if (bh.count)
      npt_cs_decoder_read(dec, bh.count * entry_size, entries,
                          bh.count * entry_size);
   if (npt_cs_decoder_get_fatal(dec))
      return;

   switch (bh.kind) {
   case NPT_DESC_BATCH_COPY_SIMPLE:
      npt_desc_batch_copy_simple(dispatch, device, bh.heap_type,
                                 entries, bh.count);
      break;
   case NPT_DESC_BATCH_CREATE_CBV:
      npt_desc_batch_create_cbv(dispatch, device, entries, bh.count);
      break;
   }
   return;

malformed:
   npt_log("malformed DESC_BATCH: kind=%u heap=%u count=%u cmd_size=%u",
           bh.kind, bh.heap_type, bh.count, header->cmd_size);
   npt_cs_decoder_set_fatal(dec);
}
