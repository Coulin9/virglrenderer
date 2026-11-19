/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef NPT_DESC_BATCH_H
#define NPT_DESC_BATCH_H

#include "neptune-protocol/npt_protocol_transport.h"

struct npt_context;
struct npt_dispatch_context;
struct npt_cs_decoder;

void
npt_transport_desc_batch(struct npt_context *ctx,
                         struct npt_dispatch_context *dispatch,
                         struct npt_cs_decoder *dec,
                         const struct npt_command_header *header);

#endif /* NPT_DESC_BATCH_H */
