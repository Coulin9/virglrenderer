/*
 * Copyright 2026
 * SPDX-License-Identifier: MIT
 */

#include "proxy_doorbell.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>

struct proxy_doorbell_slot {
   pthread_rwlock_t lock;
   struct render_ring_control *ctl;
   union render_ring_line *state;
   render_wake_t wake;
};

static struct proxy_doorbell_slot
   proxy_doorbell_slots[PROXY_DOORBELL_SLOT_COUNT];
static pthread_once_t proxy_doorbell_once = PTHREAD_ONCE_INIT;

static void
proxy_doorbell_init(void)
{
   for (uint32_t i = 0; i < PROXY_DOORBELL_SLOT_COUNT; i++) {
      pthread_rwlock_init(&proxy_doorbell_slots[i].lock, NULL);
      proxy_doorbell_slots[i].ctl = NULL;
      proxy_doorbell_slots[i].state = NULL;
      proxy_doorbell_slots[i].wake = RENDER_WAKE_NULL;
   }
}

void
proxy_doorbell_register(uint32_t ctx_id,
                        struct render_ring_control *ctl,
                        union render_ring_line *state,
                        render_wake_t wake)
{
   if (ctx_id >= PROXY_DOORBELL_SLOT_COUNT)
      return;

   pthread_once(&proxy_doorbell_once, proxy_doorbell_init);
   struct proxy_doorbell_slot *s = &proxy_doorbell_slots[ctx_id];
   pthread_rwlock_wrlock(&s->lock);
   s->ctl = ctl;
   s->state = state;
   s->wake = wake;
   pthread_rwlock_unlock(&s->lock);
}

void
proxy_doorbell_unregister(uint32_t ctx_id)
{
   if (ctx_id >= PROXY_DOORBELL_SLOT_COUNT)
      return;

   pthread_once(&proxy_doorbell_once, proxy_doorbell_init);
   struct proxy_doorbell_slot *s = &proxy_doorbell_slots[ctx_id];
   pthread_rwlock_wrlock(&s->lock);
   s->ctl = NULL;
   s->state = NULL;
   s->wake = RENDER_WAKE_NULL;
   pthread_rwlock_unlock(&s->lock);
}

int
proxy_doorbell_ring(uint32_t ctx_id, uint32_t ring_id)
{
   if (ctx_id == 0 || ctx_id >= PROXY_DOORBELL_SLOT_COUNT ||
       ring_id >= RENDER_RING_DOORBELL_MAX_RINGS)
      return -EINVAL;

   pthread_once(&proxy_doorbell_once, proxy_doorbell_init);
   struct proxy_doorbell_slot *s = &proxy_doorbell_slots[ctx_id];
   pthread_rwlock_rdlock(&s->lock);
   if (!s->ctl) {
      pthread_rwlock_unlock(&s->lock);
      return -EINVAL;
   }
   render_ring_doorbell_set(s->ctl, ring_id, s->state, s->wake);
   pthread_rwlock_unlock(&s->lock);
   return 0;
}
