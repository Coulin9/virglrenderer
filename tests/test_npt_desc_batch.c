/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * DESC_BATCH decode: a valid batch reaches the device vtable once per
 * kind (a single CopyDescriptors for a copy run, per-entry
 * CreateConstantBufferView for CBVs); malformed kind, count/size
 * mismatch and out-of-range heap type are fatal; an unknown device
 * drops like the generated reply-less dispatch.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "neptune/npt_cs.h"
#include "neptune/npt_desc_batch.h"
#include "neptune/neptune-protocol/npt_protocol_host_id3d12device.h"

static int failures;

#define CHECK(cond)                                                          \
   do {                                                                      \
      if (!(cond)) {                                                         \
         fprintf(stderr, "test_npt_desc_batch: FAIL %s:%d: %s\n",            \
                __func__, __LINE__, #cond);                                  \
         failures++;                                                         \
      }                                                                      \
   } while (0)

/* ---------------- stubs ---------------- */

void
npt_log(const char *fmt, ...)
{
   (void)fmt;
}

void
_debug_assert_fail(const char *expr, const char *file, unsigned line,
                   const char *function)
{
   fprintf(stderr, "test_npt_desc_batch: assert %s failed at %s:%u (%s)\n",
           expr, file, line, function);
   abort();
}

static void *lookup_result;
static uint64_t lookup_id;
static bool lookup_called;

void *
npt_cs_handle_lookup(struct npt_dispatch_context *ctx,
                     npt_object_id id, npt_object_type type)
{
   (void)ctx;
   lookup_called = true;
   lookup_id = id;
   CHECK(type == NPT_OBJECT_TYPE_ID3D12DEVICE);
   return lookup_result;
}

/* ---------------- fake device vtable ---------------- */

static unsigned copy_calls;
static UINT copy_ndst, copy_nsrc;
static D3D12_DESCRIPTOR_HEAP_TYPE copy_heap;
static D3D12_CPU_DESCRIPTOR_HANDLE copy_dst0, copy_src0;
static UINT copy_dst_num0, copy_src_num0;

static void NPT_STDMETHODCALLTYPE
fake_CopyDescriptors(void *self, UINT nd,
                     const D3D12_CPU_DESCRIPTOR_HANDLE *dst,
                     const UINT *ds, UINT ns,
                     const D3D12_CPU_DESCRIPTOR_HANDLE *src,
                     const UINT *ss, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
   (void)self;
   copy_calls++;
   copy_ndst = nd;
   copy_nsrc = ns;
   copy_heap = type;
   copy_dst0 = dst[0];
   copy_src0 = src[0];
   copy_dst_num0 = ds[0];
   copy_src_num0 = ss[0];
}

static unsigned cbv_calls;
static D3D12_CPU_DESCRIPTOR_HANDLE cbv_last_dst;
static D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_last_desc;
static bool cbv_last_null;

static void NPT_STDMETHODCALLTYPE
fake_CreateConstantBufferView(void *self,
                              const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc,
                              D3D12_CPU_DESCRIPTOR_HANDLE dst)
{
   (void)self;
   cbv_calls++;
   cbv_last_dst = dst;
   cbv_last_null = !desc;
   if (desc)
      cbv_last_desc = *desc;
}

static void *fake_vtbl[64];
static struct {
   void **vtbl;
} fake_device = { fake_vtbl };

/* ---------------- driver ---------------- */

static bool fatal;
static struct npt_cs_decoder dec;
static struct npt_dispatch_context dispatch;

static uint8_t cmd[1024];

static void
build_batch(uint32_t kind, uint32_t heap, uint32_t count,
            const void *entries, uint32_t entry_size, uint32_t *out_size)
{
   struct npt_cmd_desc_batch bh = {
      .header = {
         .cmd_type = NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_DESC,
                                            NPT_TRANSPORT_DESC_BATCH),
         .cmd_size = (uint32_t)(sizeof(bh) + count * entry_size),
      },
      .device_id = 0xdeadbeef,
      .kind = kind,
      .heap_type = heap,
      .count = count,
   };
   memcpy(cmd, &bh, sizeof(bh));
   if (count)
      memcpy(cmd + sizeof(bh), entries, count * entry_size);
   *out_size = bh.header.cmd_size;
}

/* Position the decoder past the header, as the ring dispatch leaves it
 * when it calls the transport handler. */
static void
run_batch(const struct npt_command_header *hdr)
{
   struct npt_command_header consumed;
   npt_cs_decoder_set_buffer_stream(&dec, cmd, hdr->cmd_size);
   npt_cs_decoder_read(&dec, sizeof(consumed), &consumed, sizeof(consumed));
   npt_transport_desc_batch(NULL, &dispatch, &dec, &consumed);
}

int
main(void)
{
   npt_cs_decoder_init(&dec, &fatal);
   memset(&dispatch, 0, sizeof(dispatch));
   dispatch.decoder = &dec;
   fake_vtbl[NPT_VTBL_ID3D12Device_CopyDescriptors] =
      (void *)&fake_CopyDescriptors;
   fake_vtbl[NPT_VTBL_ID3D12Device_CreateConstantBufferView] =
      (void *)&fake_CreateConstantBufferView;
   lookup_result = &fake_device;

   /* Valid COPY_SIMPLE batch: one host CopyDescriptors with per-entry
    * dst/src ranges and sizes. */
   {
      const struct npt_desc_copy_entry es[2] = {
         { 0x1000, 0x8000, 2, 0 },
         { 0x2000, 0x9000, 3, 0 },
      };
      uint32_t size;
      build_batch(NPT_DESC_BATCH_COPY_SIMPLE,
                  D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, es,
                  sizeof(es[0]), &size);
      lookup_called = false;
      run_batch((const struct npt_command_header *)cmd);
      CHECK(!fatal);
      CHECK(lookup_called && lookup_id == 0xdeadbeef);
      CHECK(copy_calls == 1 && cbv_calls == 0);
      CHECK(copy_ndst == 2 && copy_nsrc == 2);
      CHECK(copy_heap == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      CHECK(copy_dst0.ptr == 0x1000 && copy_src0.ptr == 0x8000);
      CHECK(copy_dst_num0 == 2 && copy_src_num0 == 2);
   }

   /* Valid CBV batch: per-entry host calls; has_desc == 0 maps to a
    * NULL desc. */
   {
      const struct npt_desc_cbv_entry es[2] = {
         { 0x400000, 256, 1, 0x5000 },
         { 0, 0, 0, 0x6000 },
      };
      uint32_t size;
      build_batch(NPT_DESC_BATCH_CREATE_CBV, 0, 2, es, sizeof(es[0]),
                  &size);
      run_batch((const struct npt_command_header *)cmd);
      CHECK(!fatal);
      CHECK(cbv_calls == 2);
      CHECK(cbv_last_null && cbv_last_dst.ptr == 0x6000);
   }

   /* count / cmd_size mismatch is fatal. */
   {
      const struct npt_desc_copy_entry es[1] = {
         { 0x1000, 0x8000, 1, 0 },
      };
      uint32_t size;
      build_batch(NPT_DESC_BATCH_COPY_SIMPLE, 0, 1, es, sizeof(es[0]),
                  &size);
      struct npt_command_header *hdr =
         (struct npt_command_header *)cmd;
      hdr->cmd_size += sizeof(es[0]);
      const unsigned calls_before = copy_calls;
      run_batch(hdr);
      CHECK(fatal);
      CHECK(copy_calls == calls_before);
      fatal = false;
   }

   /* Unknown kind is fatal. */
   {
      uint32_t size;
      build_batch(99, 0, 0, NULL, 0, &size);
      run_batch((const struct npt_command_header *)cmd);
      CHECK(fatal);
      fatal = false;
   }

   /* Out-of-range heap type is fatal. */
   {
      const struct npt_desc_copy_entry es[1] = {
         { 0x1000, 0x8000, 1, 0 },
      };
      uint32_t size;
      build_batch(NPT_DESC_BATCH_COPY_SIMPLE,
                  D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES, 1, es,
                  sizeof(es[0]), &size);
      run_batch((const struct npt_command_header *)cmd);
      CHECK(fatal);
      fatal = false;
   }

   /* An unregistered device drops without dispatching, like the
    * generated reply-less path. */
   {
      const struct npt_desc_cbv_entry es[1] = {
         { 0x400000, 256, 1, 0x5000 },
      };
      uint32_t size;
      build_batch(NPT_DESC_BATCH_CREATE_CBV, 0, 1, es, sizeof(es[0]),
                  &size);
      lookup_result = NULL;
      const unsigned calls_before = cbv_calls;
      run_batch((const struct npt_command_header *)cmd);
      CHECK(!fatal);
      CHECK(cbv_calls == calls_before);
      lookup_result = &fake_device;
   }

   npt_cs_decoder_fini(&dec);
   if (failures)
      fprintf(stderr, "test_npt_desc_batch: %d failure(s)\n", failures);
   else
      puts("test_npt_desc_batch: PASS");
   return failures ? 1 : 0;
}
