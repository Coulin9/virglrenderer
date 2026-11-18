/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D12 fence dispatch overrides.  Both Signal entry points feed the
 * feedback substrate so the guest can read a fence's value out of a
 * shared slot instead of over the wire: ID3D12CommandQueue::Signal is
 * the GPU-timeline advance, ID3D12Fence::Signal the CPU-side one.
 */

#include "npt_com.h"
#include "npt_context.h"
#include "npt_event.h"
#include "npt_feedback.h"
#include "npt_overrides.h"
#include "npt_ring.h"
#include "npt_work_context.h"

#include "neptune-protocol/npt_protocol_host_dispatch_types.h"
#include "neptune-protocol/npt_protocol_host_id3d12commandqueue.h"
#include "neptune-protocol/npt_protocol_host_id3d12device.h"
#include "neptune-protocol/npt_protocol_host_id3d12fence.h"

/* ---- GATE_WAIT bridge ---------------------------------------------------- */

void
npt_d3d12_gate_addref(void *fence)
{
   npt_com_add_ref(fence);
}

void
npt_d3d12_gate_release(void *fence)
{
   npt_com_release(fence);
}

bool
npt_d3d12_gate_seoc(void *fence, uint64_t value, void *signal_handle)
{
   /* signal_handle follows the same convention as the event-proxy path:
    * whatever npt_event's event_fd_signal_handle produces for this
    * platform. */
   PFN_ID3D12Fence_SetEventOnCompletion seoc = NPT_COM_VTBL_FUNC(
      PFN_ID3D12Fence_SetEventOnCompletion, npt_com_vtable(fence),
      NPT_VTBL_ID3D12Fence_SetEventOnCompletion);
   return NPT_SUCCEEDED(seoc(fence, value, (HANDLE)signal_handle));
}

bool
npt_d3d12_gate_reached(void *fence, uint64_t value)
{
   PFN_ID3D12Fence_GetCompletedValue get = NPT_COM_VTBL_FUNC(
      PFN_ID3D12Fence_GetCompletedValue, npt_com_vtable(fence),
      NPT_VTBL_ID3D12Fence_GetCompletedValue);
   return get(fence) >= value;
}

static HRESULT
npt_override_ID3D12CommandQueue_Signal(
   struct npt_dispatch_context *dctx,
   struct npt_command_ID3D12CommandQueue_Signal *args,
   PFN_ID3D12CommandQueue_Signal original)
{
   args->ret = original(args->_self, args->pFence, args->Value);

   if (NPT_SUCCEEDED(args->ret) && args->pFence) {
      npt_feedback_fence_mark_signal(npt_context_from_dispatch(dctx),
                                     args->pFence, args->Value,
                                     (uintptr_t)args->_self);
   }
   return args->ret;
}

struct npt_execute_call {
   struct npt_command_ID3D12CommandQueue_ExecuteCommandLists *args;
   PFN_ID3D12CommandQueue_ExecuteCommandLists original;
};

static void
npt_execute_original(void *argument)
{
   struct npt_execute_call *call = argument;
   call->original(call->args->_self, call->args->NumCommandLists,
                  call->args->ppCommandLists);
}

static void
npt_override_ID3D12CommandQueue_ExecuteCommandLists(
   struct npt_dispatch_context *dctx,
   struct npt_command_ID3D12CommandQueue_ExecuteCommandLists *args,
   PFN_ID3D12CommandQueue_ExecuteCommandLists original)
{
   struct npt_execute_call call = { args, original };
   struct npt_ring *ring = npt_ring_current();
   if (ring && &ring->dispatch != dctx)
      ring = NULL;

   struct npt_context *ctx = npt_context_from_dispatch(dctx);
   /* The ring publishes head after each command, so it still names this
    * ExecuteCommandLists while its decoded arguments are valid here. */
   npt_work_context_execute(ctx->work_context, ring ? ring->id : 0,
                            ring ? npt_ring_load_head(ring) : 0,
                            args->_self, args->NumCommandLists,
                            npt_execute_original, &call);
}

static HRESULT
npt_override_ID3D12Fence_Signal(
   struct npt_dispatch_context *dctx,
   struct npt_command_ID3D12Fence_Signal *args,
   PFN_ID3D12Fence_Signal original)
{
   args->ret = original(args->_self, args->Value);

   if (NPT_SUCCEEDED(args->ret)) {
      struct npt_context *ctx = npt_context_from_dispatch(dctx);
      npt_feedback_fence_mark_signal(ctx, args->_self, args->Value, 0);
      /* CPU Signal completes before original returns, so the slot is
       * current by the time the reply reaches the guest. */
      npt_feedback_fence_refresh(ctx, args->_self);
   }
   return args->ret;
}

static HRESULT
npt_override_ID3D12Fence_SetEventOnCompletion(
   struct npt_dispatch_context *dctx,
   struct npt_command_ID3D12Fence_SetEventOnCompletion *args,
   PFN_ID3D12Fence_SetEventOnCompletion original)
{
   /* Recorded before the arm: the backend may signal the proxy inside
    * original() when the fence already reached Value, and the sync queue
    * must find the fence to refresh its slot.  Rolled back on failure. */
   struct npt_event_proxy *pr = NULL;
   struct npt_context *ctx = npt_context_from_dispatch(dctx);
   if (args->hEvent) {
      pr = npt_event_command_proxy();
      if (pr)
         npt_event_note_seoc_fences(ctx, pr, &args->_self, 1);
   }

   args->ret = original(args->_self, args->Value, args->hEvent);

   if (pr && !NPT_SUCCEEDED(args->ret))
      npt_event_drop_seoc_fences(ctx, pr);
   return args->ret;
}

static HRESULT
npt_override_ID3D12Device1_SetEventOnMultipleFenceCompletion(
   struct npt_dispatch_context *dctx,
   struct npt_command_ID3D12Device1_SetEventOnMultipleFenceCompletion *args,
   PFN_ID3D12Device1_SetEventOnMultipleFenceCompletion original)
{
   struct npt_event_proxy *pr = NULL;
   struct npt_context *ctx = npt_context_from_dispatch(dctx);
   if (args->hEvent) {
      pr = npt_event_command_proxy();
      if (pr)
         npt_event_note_seoc_fences(ctx, pr, (void *const *)args->ppFences,
                                    args->NumFences);
   }

   args->ret = original(args->_self, args->ppFences, args->pFenceValues,
                        args->NumFences, args->Flags, args->hEvent);

   if (pr && !NPT_SUCCEEDED(args->ret))
      npt_event_drop_seoc_fences(ctx, pr);
   return args->ret;
}

struct npt_dispatch_id3d12commandqueue_overrides
npt_id3d12commandqueue_overrides = {
   .ExecuteCommandLists = npt_override_ID3D12CommandQueue_ExecuteCommandLists,
   .Signal = npt_override_ID3D12CommandQueue_Signal,
};

struct npt_dispatch_id3d12fence_overrides npt_id3d12fence_overrides = {
   .SetEventOnCompletion = npt_override_ID3D12Fence_SetEventOnCompletion,
   .Signal = npt_override_ID3D12Fence_Signal,
};

struct npt_dispatch_id3d12device1_overrides npt_id3d12device1_overrides = {
   .SetEventOnMultipleFenceCompletion =
      npt_override_ID3D12Device1_SetEventOnMultipleFenceCompletion,
};
