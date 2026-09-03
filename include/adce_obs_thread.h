#ifndef ADCE_OBS_THREAD_H
#define ADCE_OBS_THREAD_H

#include "adce_observe.h"

#if defined(__cplusplus)
extern "C" {
#endif

/* ===========================================================================
 * The observer thread: the scheduling half of the Observation Plane.
 *
 * Optional by construction. A consumer that owns its own scheduling -- an
 * existing event loop, a timer wheel, a real-time framework -- can drive
 * adce_obs_epoch_close() directly and never link this translation unit. It
 * lives in the library rather than in a test because otherwise every consumer
 * reimplements epoch cadence and writer ownership, and the
 * T <= ADCE_ADVICE_TIMEOUT_NS / 2 invariant stops being enforced by the
 * _Static_assert in adce_observe.h that currently locks it.
 *
 * It changes nothing about testability. adce_obs_epoch_close still takes
 * observed_at_ns as a parameter; this thread is simply the caller that passes
 * a real clock reading. Every property the unit tests assert is asserted
 * without it.
 * ===========================================================================
 */

/* Stop latency is bounded by this rather than by the epoch period: the loop
 * sleeps toward its deadline in slices and re-reads the stop flag at each
 * one, so a stop request is observed within a tick instead of after up to a
 * full T. */
#define ADCE_OBS_THREAD_TICK_NS (1000ULL * 1000ULL) /* 1 ms */

/* An epoch closed within this much of its deadline is on time. Without a
 * tolerance every epoch would count as late, since a wakeup is never exact
 * and the count would carry no information. */
#define ADCE_OBS_THREAD_LATE_NS (ADCE_OBS_EPOCH_NS / 10)

typedef struct {
    /* Owned by the observer thread once it has claimed the writer. Read by
     * other threads only after adce_obs_thread_stop() has joined. */
    adce_obs_ctx_t ctx;

    pthread_t thread;
    int started;

    _Atomic int running;
    /* 0 while the claim is pending, 1 once the observer thread owns the
     * writer, -1 if the claim was refused. Makes start synchronous. */
    _Atomic int ready;

    /* Scheduling diagnostics, written only by the observer thread. */
    uint64_t late_epochs;
    uint64_t skipped_epochs;

    /* Arrivals dropped by the missed-epoch branch in obs_service_deadline.
     * This SYSTEMATICALLY UNDER-REPORTS what the stall actually cost: the
     * stall suppresses the ingress rate itself (backpressure, timeouts,
     * whatever the caller does under a non-responding observer), so fewer
     * arrivals are tapped in the first place and this counts only the ones
     * that still made it to the counter. Measured across captured runs at
     * 5.7% to 49% of the nominal (un-stalled) arrival rate for the
     * corresponding interval. Treat this field as a floor on the loss, not
     * the loss. */
    uint64_t discarded_arrivals;
} adce_obs_thread_t;

/* Initialises the context, starts the thread, and does not return until the
 * writer claim has succeeded or failed. Returns 1 on success, 0 if the thread
 * could not be created or the claim was refused -- a refused claim means
 * another writer already owns this context, which must fail loudly at startup
 * rather than as a silently non-publishing thread. */
int adce_obs_thread_start(adce_obs_thread_t *t, adce_obs_counter_t *counter,
                          adce_epoch_state_t *epoch);

/* Requests the stop and joins. Safe against a publication in flight; see the
 * implementation for why, and for why pthread_cancel is not used. Idempotent. */
void adce_obs_thread_stop(adce_obs_thread_t *t);

#if defined(__cplusplus)
}
#endif

#endif /* ADCE_OBS_THREAD_H */
