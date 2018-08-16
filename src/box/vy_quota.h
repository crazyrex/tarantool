#ifndef INCLUDES_TARANTOOL_BOX_VY_QUOTA_H
#define INCLUDES_TARANTOOL_BOX_VY_QUOTA_H
/*
 * Copyright 2010-2017, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdbool.h>
#include <stddef.h>
#include <tarantool_ev.h>

#include "fiber.h"
#include "fiber_cond.h"
#include "say.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

struct vy_quota;

typedef void
(*vy_quota_exceeded_f)(struct vy_quota *quota);

/**
 * Quota used for accounting and limiting memory consumption
 * in the vinyl engine. It is NOT multi-threading safe.
 */
struct vy_quota {
	/**
	 * Memory limit. Once hit, new transactions are
	 * throttled until memory is reclaimed.
	 */
	size_t limit;
	/**
	 * Memory watermark. Exceeding it does not result in
	 * throttling new transactions, but it does trigger
	 * background memory reclaim.
	 */
	size_t watermark;
	/** Current memory consumption. */
	size_t used;
	/**
	 * If vy_quota_try_use() takes longer than the given
	 * value, warn about it in the log.
	 */
	double too_long_threshold;
	/**
	 * Condition variable used for throttling consumers when
	 * there is no quota left.
	 */
	struct fiber_cond cond;
	/**
	 * Called when quota is consumed if used >= watermark.
	 * It is supposed to trigger memory reclaim.
	 */
	vy_quota_exceeded_f quota_exceeded_cb;
};

static inline void
vy_quota_create(struct vy_quota *q, vy_quota_exceeded_f quota_exceeded_cb)
{
	q->limit = SIZE_MAX;
	q->watermark = SIZE_MAX;
	q->used = 0;
	q->too_long_threshold = TIMEOUT_INFINITY;
	q->quota_exceeded_cb = quota_exceeded_cb;
	fiber_cond_create(&q->cond);
}

static inline void
vy_quota_destroy(struct vy_quota *q)
{
	fiber_cond_broadcast(&q->cond);
	fiber_cond_destroy(&q->cond);
}

/**
 * Set memory limit. If current memory usage exceeds
 * the new limit, invoke the callback.
 */
static inline void
vy_quota_set_limit(struct vy_quota *q, size_t limit)
{
	q->limit = q->watermark = limit;
	if (q->used >= limit)
		q->quota_exceeded_cb(q);
	fiber_cond_broadcast(&q->cond);
}

/**
 * Set memory watermark. If current memory usage exceeds
 * the new watermark, invoke the callback.
 */
static inline void
vy_quota_set_watermark(struct vy_quota *q, size_t watermark)
{
	q->watermark = watermark;
	if (q->used >= watermark)
		q->quota_exceeded_cb(q);
}

/**
 * Consume @size bytes of memory. In contrast to vy_quota_try_use()
 * this function does not throttle the caller.
 */
static inline void
vy_quota_force_use(struct vy_quota *q, size_t size)
{
	q->used += size;
	if (q->used >= q->watermark)
		q->quota_exceeded_cb(q);
}

/**
 * Function called on dump completion to release quota after
 * freeing memory.
 */
static inline void
vy_quota_dump(struct vy_quota *q, size_t size)
{
	assert(q->used >= size);
	q->used -= size;
	fiber_cond_broadcast(&q->cond);
}

/**
 * Try to consume @size bytes of memory, throttle the caller
 * if the limit is exceeded. @timeout specifies the maximal
 * time to wait. Return 0 on success, -1 on timeout.
 *
 * Usage pattern:
 *
 *   size_t reserved = <estimate>;
 *   if (vy_quota_try_use(q, reserved, timeout) != 0)
 *           return -1;
 *   <allocate memory>
 *   size_t used = <actually allocated>;
 *   vy_quota_commit_use(q, reserved, used);
 *
 * We use two-step quota allocation strategy (reserve-consume),
 * because we may not yield after we start inserting statements
 * into a space so we estimate the allocation size and wait for
 * quota before committing statements. At the same time, we
 * cannot precisely estimate the size of memory we are going to
 * consume so we adjust the quota after the allocation.
 *
 * The size of memory allocated while committing a transaction
 * may be greater than an estimate, because insertion of a
 * statement into an in-memory index can trigger allocation
 * of a new index extent. This should not normally result in a
 * noticeable breach in the memory limit, because most memory
 * is occupied by statements, but we need to adjust the quota
 * accordingly after the allocation in this case.
 *
 * The actual memory allocation size may also be less than an
 * estimate if the space has multiple indexes, because statements
 * are stored in the common memory level, which isn't taken into
 * account while estimating the size of a memory allocation.
 */
static inline int
vy_quota_try_use(struct vy_quota *q, size_t size, double timeout)
{
	double start_time = ev_monotonic_now(loop());
	double deadline = start_time + timeout;
	while (q->used + size > q->limit && timeout > 0) {
		q->quota_exceeded_cb(q);
		if (fiber_cond_wait_deadline(&q->cond, deadline) != 0)
			break; /* timed out */
	}
	double wait_time = ev_monotonic_now(loop()) - start_time;
	if (wait_time > q->too_long_threshold) {
		say_warn("waited for %zu bytes of vinyl memory quota "
			 "for too long: %.3f sec", size, wait_time);
	}
	if (q->used + size > q->limit)
		return -1;
	q->used += size;
	if (q->used >= q->watermark)
		q->quota_exceeded_cb(q);
	return 0;
}

/**
 * Adjust quota after allocating memory.
 *
 * @reserved: size of quota reserved by vy_quota_try_use().
 * @used: size of memory actually allocated.
 *
 * See also vy_quota_try_use().
 */
static inline void
vy_quota_commit_use(struct vy_quota *q, size_t reserved, size_t used)
{
	if (reserved > used) {
		size_t excess = reserved - used;
		assert(q->used >= excess);
		q->used -= excess;
		fiber_cond_broadcast(&q->cond);
	}
	if (reserved < used)
		vy_quota_force_use(q, used - reserved);
}

/**
 * Block the caller until the quota is not exceeded.
 */
static inline void
vy_quota_wait(struct vy_quota *q)
{
	while (q->used > q->limit)
		fiber_cond_wait(&q->cond);
}

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

#endif /* INCLUDES_TARANTOOL_BOX_VY_QUOTA_H */
