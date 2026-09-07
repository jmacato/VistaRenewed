/*
 * Copyright 2023 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef VIRGL_FENCE_H
#define VIRGL_FENCE_H

#include <stdint.h>
#include <unistd.h>

int virgl_fence_table_init(void);
void virgl_fence_table_cleanup(void);
int virgl_fence_set_fd(uint64_t fence_id, int fd);
int virgl_fence_get_fd(uint64_t fence_id);
int virgl_fence_get_last_signalled_fence_fd(void);

/*
 * Entries are otherwise only dropped by the opportunistic sweep in
 * virgl_fence_set_fd(), which reaps whatever has become readable -- so an fd
 * that never signals is never reaped.  Whoever knows an entry is finished with
 * says so with one of these: take_fd hands the fd over (one-shot hand-off to a
 * single known consumer), retire just drops it (the fence is done, by whatever
 * means, and nobody will ask for its fd again).
 */
int virgl_fence_take_fd(uint64_t fence_id);
void virgl_fence_retire(uint64_t fence_id);

/*
 * virtio-gpu fences live on per-ring timelines, so a fence's identity is the
 * (ring_index, seqno) pair: the seqno alone repeats across rings (every ring's
 * timeline starts at 1).  The render-server fd hand-off registers a fence fd
 * with virgl_fence_set_fd() and reads it back with virgl_fence_get_fd(), so
 * both sides must key that table by the full pair -- keying by seqno alone lets
 * fences on different rings share a key.  Fold the pair into one table key with
 * this helper.
 *
 * Layout: ring_index in the high 16 bits (timeline_count caps it far below
 * 2^16), seqno in the low 48 (a per-ring counter that never approaches 2^48).
 * The VMM-side table (proxy / public API) instead keys by a globally-unique
 * client fence_id and passes that unfolded.
 */
static inline uint64_t
virgl_fence_ring_key(uint32_t ring_index, uint64_t seqno)
{
   return ((uint64_t)ring_index << 48) | (seqno & 0xffffffffffffull);
}

#endif /* VIRGL_FENCE_H */
