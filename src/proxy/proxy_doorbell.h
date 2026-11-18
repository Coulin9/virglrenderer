/*
 * Copyright 2026
 * SPDX-License-Identifier: MIT
 */

#ifndef PROXY_DOORBELL_H
#define PROXY_DOORBELL_H

#include <stdint.h>

#include "../../server/render_ring.h"

/* MMIO doorbell slot table: the VMM calls proxy_doorbell_ring() directly
 * from the vCPU thread that took the guest's MMIO write.  Slots are
 * indexed by ctx_id; context init fills its slot and destroy clears
 * it before the ring shm is unmapped, both
 * under the slot's write lock, while ringers hold the read lock. */

#define PROXY_DOORBELL_SLOT_COUNT 256

void
proxy_doorbell_register(uint32_t ctx_id,
                        struct render_ring_control *ctl,
                        union render_ring_line *state,
                        render_wake_t wake);

void
proxy_doorbell_unregister(uint32_t ctx_id);

int
proxy_doorbell_ring(uint32_t ctx_id, uint32_t ring_id);

#endif /* PROXY_DOORBELL_H */
