/*
 * Copyright 2026
 * SPDX-License-Identifier: MIT
 */

#ifndef RENDER_RING_H
#define RENDER_RING_H

/* Shared memory submit ring between the VMM proxy (src/proxy) and the render
 * worker (server/).  One memfd-backed region per context replaces the old
 * timeline-only shmem and the fence socket:
 *
 *   offset 0                         struct render_ring_control
 *   control->main ring offset        byte ring (pow2, entries never wrap)
 *   control->oob ring offset         fixed-slot fence ring (Neptune only)
 *
 * All three directions (VMM->worker main, VMM->worker oob, worker->VMM
 * completion) use the same conditional-wake state machine:
 *
 *   consumer: drain; optional bounded spin; store SLEEPING (relaxed);
 *   seq_cst fence; re-check for work -> if any, store ACTIVE and keep
 *   draining; else poll the wake fd; drain the fd non-blockingly; store
 *   ACTIVE.
 *
 *   producer: publish (release store of tail / timeline seqno); seq_cst
 *   fence; if load(state)==SLEEPING && CAS(SLEEPING->WAKING), signal the
 *   wake fd once.  Wake fds are counting-tolerant: a stale byte only costs
 *   one early return.
 */

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RENDER_RING_CACHELINE 128
/* matches virtio-gpu / PROXY_CONTEXT_TIMELINE_COUNT */
#define RENDER_RING_TIMELINE_COUNT 64
#define RENDER_RING_MAIN_SIZE (1024 * 1024)
#define RENDER_RING_OOB_SLOT_COUNT 4096
/* SUBMIT_CMD payloads larger than this travel as SUBMIT_CMD_SOCKET */
#define RENDER_RING_MAX_INLINE_CMD (256 * 1024)
/* sanity bound for socket-carried command payloads */
#define RENDER_RING_MAX_SOCKET_CMD (64 * 1024 * 1024)
#define RENDER_RING_DEFAULT_SPIN_NS 2000

#define RENDER_RING_FLAG_ALWAYS_NOTIFY 0x1
#define RENDER_RING_FLAG_LATENCY_TRACE 0x2

/* Per-fence latency trace (VIRGL_LATENCY_TRACE on the proxy): when the
 * flag is set the shared region carries a render_ring_lat_area after the
 * rings.  Records are indexed by fence_id % RENDER_RING_LAT_RECORDS;
 * fence_id == 0 means the slot is unused. */
#define RENDER_RING_LAT_RECORDS 65536

struct render_ring_lat_rec {
   _Atomic uint64_t fence_id; /* written by the VMM at submit time */
   uint64_t t_arrive;         /* VMM: submit_fence entry */
   uint64_t t_pushed;         /* VMM: ring push + wake returned */
   uint64_t t_worker_recv;    /* worker: fence entry dequeued */
   uint64_t t_worker_signal;  /* worker: just before timeline publish */
   uint64_t t_vmm_wake;       /* VMM: poll() return that led to retire */
   uint64_t t_vmm_cb_begin;   /* VMM: before the fence callback */
   uint64_t t_vmm_cb_end;     /* VMM: after the fence callback */
};

static_assert(sizeof(struct render_ring_lat_rec) == 64,
              "latency record must stay 64 bytes");

/* (ring_idx, seqno) -> fence_id side map the proxy publishes next to
 * the latency records so the worker can stamp them without the ring
 * payload carrying the id.  The plain ring formats stay untouched when
 * tracing is off.  Indexed by ring_idx * STRIDE + (seqno & (STRIDE-1));
 * a slot is valid when its seqno matches. */
#define RENDER_RING_LAT_SEQMAP_STRIDE 1024

struct render_ring_lat_seqent {
   _Atomic uint64_t fence_id; /* 0 = empty; stored last (release) */
   _Atomic uint32_t seqno;
   uint32_t _pad;
};

static_assert(sizeof(struct render_ring_lat_seqent) == 16,
              "seqmap entry must stay 16 bytes");

struct render_ring_lat_area {
   struct render_ring_lat_rec recs[RENDER_RING_LAT_RECORDS];
   uint32_t ring_idx[RENDER_RING_LAT_RECORDS];
   uint32_t path[RENDER_RING_LAT_RECORDS]; /* 0 = main ring, 1 = oob ring */
   struct render_ring_lat_seqent
      seqmap[RENDER_RING_TIMELINE_COUNT][RENDER_RING_LAT_SEQMAP_STRIDE];
};

static_assert(RENDER_RING_LAT_RECORDS == (1u << 16),
              "record count must be a power of two");

/* monotonic-ish clock shared by both processes; under Rosetta the VMM
 * (arm64) and x86_64 workers still read the same clock source */
static inline uint64_t
render_ring_now_ns(void)
{
#if defined(__APPLE__)
   return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
   struct timespec ts;
#  if defined(__linux__) || defined(__FreeBSD__)
   clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#  else
   clock_gettime(CLOCK_MONOTONIC, &ts);
#  endif
   return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
#endif
}

enum render_ring_consumer_state {
   RENDER_RING_ACTIVE = 0,
   RENDER_RING_SLEEPING = 1,
   RENDER_RING_WAKING = 2,
};

enum render_ring_op {
   RENDER_RING_OP_PAD = 0,
   RENDER_RING_OP_SUBMIT_CMD = 1,
   RENDER_RING_OP_SUBMIT_CMD_SOCKET = 2,
   RENDER_RING_OP_SUBMIT_FENCE = 3,
   RENDER_RING_OP_SOCKET_REQUEST = 4,

   RENDER_RING_OP_MAX = RENDER_RING_OP_SOCKET_REQUEST,
};

struct render_ring_entry_header {
   uint32_t op;
   uint32_t size; /* payload bytes; entry is 8 + align8(size) */
};

/* payload of RENDER_RING_OP_SUBMIT_FENCE and of one OOB ring slot */
struct render_ring_fence {
   uint32_t flags;
   uint32_t ring_idx;
   uint32_t seqno;
};

struct render_ring_oob_slot {
   struct render_ring_fence fence;
   uint32_t _pad;
};

/* stats live on separate cache lines per writer to avoid false sharing:
 * only the VMM writes stats_vmm, only the worker writes stats_worker */
struct render_ring_stats_vmm {
   _Atomic uint64_t main_entries;
   _Atomic uint64_t main_wakes;
   _Atomic uint64_t oob_entries;
   _Atomic uint64_t oob_wakes;
   _Atomic uint64_t vmm_sleeps;
   _Atomic uint64_t backpressure_waits;
   _Atomic uint64_t socket_payload_cmds;
};

struct render_ring_stats_worker {
   _Atomic uint64_t main_sleeps;
   _Atomic uint64_t oob_sleeps;
   _Atomic uint64_t fence_notifies;
};

/* each head/tail/state word on its own cache line */
union render_ring_line {
   atomic_uint value;
   uint8_t _pad[RENDER_RING_CACHELINE];
};

struct render_ring_control {
   /* written by the worker, read by the VMM sync thread */
   atomic_uint timelines[RENDER_RING_TIMELINE_COUNT];

   union render_ring_line main_tail;  /* VMM writes */
   union render_ring_line main_head;  /* worker writes */
   union render_ring_line main_state; /* worker consumer state */

   union render_ring_line oob_tail;   /* VMM writes */
   union render_ring_line oob_head;   /* worker writes */
   union render_ring_line oob_state;  /* worker oob consumer state */

   union render_ring_line vmm_state;  /* VMM sync-thread state */
   union render_ring_line flags;      /* RENDER_RING_FLAG_* */

   union {
      struct render_ring_stats_vmm v;
      uint8_t _pad[RENDER_RING_CACHELINE];
   } stats_vmm;
   union {
      struct render_ring_stats_worker v;
      uint8_t _pad[RENDER_RING_CACHELINE];
   } stats_worker;
};

static_assert(sizeof(struct render_ring_control) % RENDER_RING_CACHELINE == 0,
              "ring control block must be cache-line sized");

static inline void
render_ring_control_init(struct render_ring_control *ctl)
{
   memset(ctl, 0, sizeof(*ctl));
}

/* Layout carried by RENDER_CONTEXT_OP_INIT.  The worker validates every
 * field against the received shmem size before touching the region. */
struct render_ring_layout {
   uint32_t main_offset;
   uint32_t main_size;   /* power of two */
   uint32_t oob_offset;
   uint32_t oob_size;    /* 0 = no oob ring */
   uint32_t lat_offset;  /* 0 = no latency trace area */
   uint32_t lat_size;
   uint32_t total_size;
};

static inline uint64_t
render_ring_align_cacheline(uint64_t v)
{
   return (v + RENDER_RING_CACHELINE - 1) &
          ~(uint64_t)(RENDER_RING_CACHELINE - 1);
}

static inline void
render_ring_layout_compute(struct render_ring_layout *layout, bool has_oob,
                           bool has_lat)
{
   layout->main_offset = (uint32_t)sizeof(struct render_ring_control);
   layout->main_size = RENDER_RING_MAIN_SIZE;
   layout->oob_offset = layout->main_offset + layout->main_size;
   layout->oob_size = has_oob ? RENDER_RING_OOB_SLOT_COUNT *
                                   (uint32_t)sizeof(struct render_ring_oob_slot)
                              : 0;
   const uint64_t rings_end = (uint64_t)layout->oob_offset + layout->oob_size;
   if (has_lat) {
      layout->lat_offset = (uint32_t)render_ring_align_cacheline(rings_end);
      layout->lat_size = (uint32_t)sizeof(struct render_ring_lat_area);
      layout->total_size = layout->lat_offset + layout->lat_size;
   } else {
      layout->lat_offset = 0;
      layout->lat_size = 0;
      layout->total_size = layout->oob_offset + layout->oob_size;
   }
}

static inline bool
render_ring_is_pow2(uint32_t v)
{
   return v && !(v & (v - 1));
}

static inline bool
render_ring_layout_valid(uint64_t shmem_size,
                         uint32_t main_offset,
                         uint32_t main_size,
                         uint32_t oob_offset,
                         uint32_t oob_size,
                         uint32_t lat_offset,
                         uint32_t lat_size)
{
   if (main_offset != sizeof(struct render_ring_control))
      return false;
   if (!render_ring_is_pow2(main_size) || main_size < 4096 ||
       main_size > 64 * 1024 * 1024)
      return false;
   uint64_t end = (uint64_t)main_offset + main_size;
   if (oob_size) {
      if (oob_offset != main_offset + main_size)
         return false;
      if (oob_size % sizeof(struct render_ring_oob_slot))
         return false;
      if (!render_ring_is_pow2(oob_size / sizeof(struct render_ring_oob_slot)))
         return false;
      end = (uint64_t)oob_offset + oob_size;
   }
   if (lat_size) {
      if (lat_size != sizeof(struct render_ring_lat_area))
         return false;
      if (lat_offset !=
          (uint32_t)render_ring_align_cacheline(end))
         return false;
      end = (uint64_t)lat_offset + lat_size;
   } else if (lat_offset) {
      return false;
   }
   return end <= shmem_size;
}

static inline struct render_ring_lat_area *
render_ring_lat_area(uint8_t *base, uint32_t offset)
{
   return offset ? (struct render_ring_lat_area *)(base + offset) : NULL;
}

static inline struct render_ring_lat_rec *
render_ring_lat_rec(struct render_ring_lat_area *lat, uint64_t fence_id)
{
   return &lat->recs[fence_id & (RENDER_RING_LAT_RECORDS - 1)];
}

static inline struct render_ring_lat_seqent *
render_ring_lat_seqmap(struct render_ring_lat_area *lat, uint32_t ring_idx,
                       uint32_t seqno)
{
   return &lat->seqmap[ring_idx][seqno & (RENDER_RING_LAT_SEQMAP_STRIDE - 1)];
}

/* ------------------------------------------------------------------ */
/* main byte ring                                                      */

struct render_ring_buf {
   atomic_uint *head; /* consumer-owned */
   atomic_uint *tail; /* producer-owned */
   uint8_t *data;
   uint32_t size; /* power of two */
};

static inline struct render_ring_buf
render_ring_main_buf(struct render_ring_control *ctl,
                     uint8_t *base,
                     uint32_t offset,
                     uint32_t size)
{
   return (struct render_ring_buf){
      .head = &ctl->main_head.value,
      .tail = &ctl->main_tail.value,
      .data = base + offset,
      .size = size,
   };
}

static inline uint32_t
render_ring_align8(uint32_t v)
{
   return (v + 7) & ~7u;
}

/* Producer: append one entry, emitting a PAD record when the contiguous
 * space to the end of the ring is too small.  Returns false when the ring
 * lacks room for the entry (caller applies the bounded back-off). */
static inline bool
render_ring_push(struct render_ring_buf *r,
                 uint32_t op,
                 const void *payload,
                 uint32_t size)
{
   const uint32_t mask = r->size - 1;
   const uint32_t entry = (uint32_t)sizeof(struct render_ring_entry_header) +
                          render_ring_align8(size);
   const uint32_t head = atomic_load_explicit(r->head, memory_order_acquire);
   uint32_t tail = atomic_load_explicit(r->tail, memory_order_relaxed);
   const uint32_t off = tail & mask;
   const uint32_t end_space = r->size - off;

   uint32_t need = entry;
   if (end_space < entry)
      need += end_space;
   if (r->size - (tail - head) < need)
      return false;

   if (end_space < entry) {
      const struct render_ring_entry_header pad = {
         .op = RENDER_RING_OP_PAD,
         .size = end_space - (uint32_t)sizeof(struct render_ring_entry_header),
      };
      memcpy(r->data + off, &pad, sizeof(pad));
      tail += end_space;
   }

   const uint32_t dst = tail & mask;
   const struct render_ring_entry_header hdr = { .op = op, .size = size };
   memcpy(r->data + dst, &hdr, sizeof(hdr));
   if (size)
      memcpy(r->data + dst + sizeof(hdr), payload, size);
   if (render_ring_align8(size) > size)
      memset(r->data + dst + sizeof(hdr) + size, 0, render_ring_align8(size) - size);

   atomic_store_explicit(r->tail, tail + entry, memory_order_release);
   return true;
}

enum render_ring_read_result {
   RENDER_RING_READ_EMPTY = 0,
   RENDER_RING_READ_OK,
   RENDER_RING_READ_CORRUPT,
};

/* Consumer: validate and fetch the next entry.  On OK, *head_io advances
 * past the entry (PAD records are skipped transparently); publish the new
 * head with render_ring_commit once the entry is consumed.  The payload
 * pointer aliases the ring: copy out whatever is needed before committing
 * if the producer could be hostile. */
static inline enum render_ring_read_result
render_ring_read(const struct render_ring_buf *r,
                 uint32_t *head_io,
                 uint32_t *op,
                 const uint8_t **payload,
                 uint32_t *size)
{
   const uint32_t mask = r->size - 1;
   const uint32_t tail = atomic_load_explicit(r->tail, memory_order_acquire);
   uint32_t head = *head_io;

   if (head == tail)
      return RENDER_RING_READ_EMPTY;
   /* head cannot lag tail by more than the ring capacity */
   if (tail - head > r->size)
      return RENDER_RING_READ_CORRUPT;

   const uint32_t off = head & mask;
   const uint32_t end_space = r->size - off;
   /* all cursors are 8-aligned so the header always fits contiguously */
   assert(end_space >= sizeof(struct render_ring_entry_header));

   struct render_ring_entry_header hdr;
   memcpy(&hdr, r->data + off, sizeof(hdr));

   if (hdr.op == RENDER_RING_OP_PAD) {
      const uint32_t pad_len =
         sizeof(hdr) + hdr.size; /* pad size is the whole pad record - hdr */
      if (hdr.size % 8 || pad_len > end_space)
         return RENDER_RING_READ_CORRUPT;
      *head_io = head + pad_len;
      return render_ring_read(r, head_io, op, payload, size);
   }

   if (hdr.op > RENDER_RING_OP_MAX)
      return RENDER_RING_READ_CORRUPT;

   /* per-op payload contract */
   switch (hdr.op) {
   case RENDER_RING_OP_SUBMIT_CMD:
      if (hdr.size > RENDER_RING_MAX_INLINE_CMD)
         return RENDER_RING_READ_CORRUPT;
      break;
   case RENDER_RING_OP_SUBMIT_CMD_SOCKET:
      if (hdr.size != sizeof(uint32_t))
         return RENDER_RING_READ_CORRUPT;
      break;
   case RENDER_RING_OP_SUBMIT_FENCE:
      if (hdr.size != sizeof(struct render_ring_fence))
         return RENDER_RING_READ_CORRUPT;
      break;
   case RENDER_RING_OP_SOCKET_REQUEST:
      if (hdr.size != 0)
         return RENDER_RING_READ_CORRUPT;
      break;
   default:
      return RENDER_RING_READ_CORRUPT;
   }

   const uint32_t entry = (uint32_t)sizeof(hdr) + render_ring_align8(hdr.size);
   /* entries never wrap and may not be torn past the published tail */
   if (entry > end_space || entry > tail - head)
      return RENDER_RING_READ_CORRUPT;

   *op = hdr.op;
   *payload = r->data + off + sizeof(hdr);
   *size = hdr.size;
   *head_io = head + entry;
   return RENDER_RING_READ_OK;
}

static inline void
render_ring_commit(const struct render_ring_buf *r, uint32_t head)
{
   atomic_store_explicit(r->head, head, memory_order_release);
}

static inline bool
render_ring_has_entries(const struct render_ring_buf *r, uint32_t head)
{
   return atomic_load_explicit(r->tail, memory_order_acquire) != head;
}

/* ------------------------------------------------------------------ */
/* oob fence ring (fixed slots)                                        */

struct render_ring_oob {
   atomic_uint *head;
   atomic_uint *tail;
   struct render_ring_oob_slot *slots;
   uint32_t count; /* power of two */
};

static inline struct render_ring_oob
render_ring_oob_buf(struct render_ring_control *ctl,
                    uint8_t *base,
                    uint32_t offset,
                    uint32_t size)
{
   return (struct render_ring_oob){
      .head = &ctl->oob_head.value,
      .tail = &ctl->oob_tail.value,
      .slots = (struct render_ring_oob_slot *)(base + offset),
      .count = size / (uint32_t)sizeof(struct render_ring_oob_slot),
   };
}

static inline bool
render_ring_oob_push(struct render_ring_oob *r,
                     const struct render_ring_fence *fence)
{
   const uint32_t head = atomic_load_explicit(r->head, memory_order_acquire);
   const uint32_t tail = atomic_load_explicit(r->tail, memory_order_relaxed);
   if (tail - head == r->count)
      return false;

   r->slots[tail & (r->count - 1)] =
      (struct render_ring_oob_slot){ .fence = *fence };
   atomic_store_explicit(r->tail, tail + 1, memory_order_release);
   return true;
}

/* Copy up to max pending slots into out[].  Returns the slot count, 0 when
 * empty, or UINT32_MAX on head/tail corruption. */
static inline uint32_t
render_ring_oob_read(const struct render_ring_oob *r,
                     uint32_t *head_io,
                     struct render_ring_oob_slot *out,
                     uint32_t max)
{
   const uint32_t mask = r->count - 1;
   const uint32_t tail = atomic_load_explicit(r->tail, memory_order_acquire);
   uint32_t head = *head_io;

   if (head == tail)
      return 0;
   if (tail - head > r->count)
      return UINT32_MAX;

   uint32_t n = 0;
   while (head != tail && n < max) {
      out[n++] = r->slots[head & mask];
      head++;
   }
   *head_io = head;
   return n;
}

static inline void
render_ring_oob_commit(const struct render_ring_oob *r, uint32_t head)
{
   atomic_store_explicit(r->head, head, memory_order_release);
}

/* ------------------------------------------------------------------ */
/* conditional wake state machine                                      */

static inline void
render_ring_fd_drain(int fd)
{
   uint8_t buf[64];
   while (read(fd, buf, sizeof(buf)) > 0) {
   }
}

static inline void
render_ring_fd_signal(int fd)
{
   const uint64_t one = 1;
   ssize_t ret;
   do {
      ret = write(fd, &one, sizeof(one));
   } while (ret < 0 && errno == EINTR);
   (void)ret; /* EAGAIN on a full pipe is harmless: it stays readable */
}

/* Producer side: call after the release store that publishes work. */
static inline void
render_ring_producer_wake(atomic_uint *state,
                          int wake_write_fd,
                          _Atomic uint64_t *wake_stat)
{
   atomic_thread_fence(memory_order_seq_cst);
   uint32_t expected = RENDER_RING_SLEEPING;
   if (atomic_load_explicit(state, memory_order_relaxed) == RENDER_RING_SLEEPING &&
       atomic_compare_exchange_strong_explicit(state, &expected, RENDER_RING_WAKING,
                                               memory_order_acq_rel,
                                               memory_order_relaxed)) {
      render_ring_fd_signal(wake_write_fd);
      if (wake_stat)
         atomic_fetch_add_explicit(wake_stat, 1, memory_order_relaxed);
   }
}

typedef bool (*render_ring_has_work_fn)(void *opaque);

enum render_ring_wait_result {
   RENDER_RING_WAIT_WORK = 0, /* work is (now) visible; go drain */
   RENDER_RING_WAIT_FD0,      /* fd0 readable without work (wake byte only) */
   RENDER_RING_WAIT_FD1,      /* fd1 readable (aux fd: socket / death pipe) */
   RENDER_RING_WAIT_TIMEOUT,  /* only when timeout_ms >= 0 */
   RENDER_RING_WAIT_ERROR,
};

/* Consumer side parking.  fd0 is the wake fd (drained after every return
 * from poll); fd1 is an optional extra fd that is reported but never
 * drained.  Caller drains work first; call this when the drain came up
 * empty. */
static inline enum render_ring_wait_result
render_ring_consumer_wait(atomic_uint *state,
                          int fd0,
                          int fd1,
                          render_ring_has_work_fn has_work,
                          void *opaque,
                          uint64_t spin_ns,
                          int timeout_ms,
                          _Atomic uint64_t *sleep_stat)
{
   if (spin_ns) {
      struct timespec start, now;
      clock_gettime(CLOCK_MONOTONIC, &start);
      do {
         if (has_work(opaque))
            return RENDER_RING_WAIT_WORK;
         clock_gettime(CLOCK_MONOTONIC, &now);
      } while ((uint64_t)(now.tv_sec - start.tv_sec) * 1000000000ull +
                  (uint64_t)(now.tv_nsec - start.tv_nsec) <
               spin_ns);
   }

   atomic_store_explicit(state, RENDER_RING_SLEEPING, memory_order_relaxed);
   atomic_thread_fence(memory_order_seq_cst);
   if (has_work(opaque)) {
      atomic_store_explicit(state, RENDER_RING_ACTIVE, memory_order_relaxed);
      return RENDER_RING_WAIT_WORK;
   }
   if (sleep_stat)
      atomic_fetch_add_explicit(sleep_stat, 1, memory_order_relaxed);

   struct pollfd fds[2] = {
      { .fd = fd0, .events = POLLIN },
      { .fd = fd1, .events = POLLIN },
   };
   const nfds_t nfds = fd1 >= 0 ? 2 : 1;

   int ret;
   do {
      ret = poll(fds, nfds, timeout_ms);
   } while (ret < 0 && errno == EINTR);

   render_ring_fd_drain(fd0);
   atomic_store_explicit(state, RENDER_RING_ACTIVE, memory_order_relaxed);

   if (ret < 0)
      return RENDER_RING_WAIT_ERROR;
   if (ret == 0)
      return RENDER_RING_WAIT_TIMEOUT;
   if (has_work(opaque))
      return RENDER_RING_WAIT_WORK;
   if (nfds > 1 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR)))
      return RENDER_RING_WAIT_FD1;
   if (fds[0].revents & POLLIN)
      return RENDER_RING_WAIT_FD0;
   return RENDER_RING_WAIT_WORK; /* spurious edge; re-drain to be safe */
}

#endif /* RENDER_RING_H */
