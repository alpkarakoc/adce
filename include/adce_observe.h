#ifndef ADCE_OBSERVE_H
#define ADCE_OBSERVE_H

/* adce_platform.h must come first: it sets the feature-test macro that makes
 * CLOCK_MONOTONIC_RAW and the entropy syscall visible, and it fails loudly if
 * a libc header beat it to the translation unit. */
#include "adce_platform.h"

#include <pthread.h>

#if defined(__cplusplus)
extern "C" {
#endif

/* ===========================================================================
 * Observation Plane: tuning constants.
 *
 * Every number the detector depends on is defined here and nowhere else. A
 * literal repeated at a use site is how T and N drift apart from the cadence
 * invariant asserted below without anything failing.
 * ===========================================================================
 */

#define ADCE_OBS_EPOCH_NS (10ULL * 1000ULL * 1000ULL) /* T = 10 ms */
#define ADCE_OBS_WINDOW_N 100                         /* N */

/* z_lo and z_hi are kept as integers as well as doubles so the ordering
 * between them is a compile-time check: the squash divides by their
 * difference, and a _Static_assert cannot evaluate a floating expression. */
#define ADCE_OBS_Z_LO_INT 3
#define ADCE_OBS_Z_HI_INT 8
#define ADCE_OBS_Z_LO ((double)ADCE_OBS_Z_LO_INT)
#define ADCE_OBS_Z_HI ((double)ADCE_OBS_Z_HI_INT)

#define ADCE_OBS_ALPHA (2.0 / ((double)ADCE_OBS_WINDOW_N + 1.0))
#define ADCE_OBS_EPOCH_SECONDS ((double)ADCE_OBS_EPOCH_NS / 1000000000.0)

/* Epsilon floor on sigma, derived rather than picked. One arrival inside one
 * epoch is the smallest rate change the measurement can express, so a sigma
 * below it is finer than the instrument: dividing by it would turn ordinary
 * quantization noise into unbounded z. Tying the floor to that configured
 * minimum rate is what keeps a quiet period from arming a hair trigger. */
#define ADCE_OBS_MIN_RATE_HZ (1.0 / ADCE_OBS_EPOCH_SECONDS)
#define ADCE_OBS_SIGMA_EPSILON ADCE_OBS_MIN_RATE_HZ

/* Cold start is fail-closed: nothing is published until the EWMA has seen a
 * full window, and the staleness watchdog -- not a second mechanism -- is what
 * makes the pre-first-publish state safe. A zero-initialised epoch state has
 * observed_at_ns == 0, so adce_epoch_is_stale() is already true. */
#define ADCE_OBS_WARMUP_EPOCHS ((uint64_t)ADCE_OBS_WINDOW_N)

/* Publication contract for `pressure`, distinct from the Q16 type's own
 * saturation bound. ADCE_Q16_MAX is correct for the type and wrong for this
 * contract; narrowing the primitive to suit one consumer would be backwards
 * coupling, so the contract gets its own name and its own clamp. */
#define ADCE_PRESSURE_MIN ((adce_q16_t)0)
#define ADCE_PRESSURE_MAX ADCE_Q16_ONE

/* Cadence invariant. At T = 10 ms against the 50 ms advice timeout there is
 * 5x margin, so four consecutive missed publications are tolerated before the
 * watchdog trips. Both sides are compile-time constants, so this is checked
 * here rather than hoped for at runtime. */
_Static_assert(ADCE_OBS_EPOCH_NS <= ADCE_ADVICE_TIMEOUT_NS / 2,
               "epoch period T must not exceed half the advice timeout");
_Static_assert(ADCE_OBS_Z_HI_INT > ADCE_OBS_Z_LO_INT,
               "z_hi must exceed z_lo: the squash divides by their difference");
_Static_assert(ADCE_OBS_WINDOW_N > 0, "EWMA window N must be positive");

/* ===========================================================================
 * The arrival counter. MANY-writer: every ingress thread taps it.
 *
 * This is a different object from adce_epoch_state_t with a different
 * concurrency contract, and keeping them physically apart is the point of the
 * padding. The epoch state has exactly one writer and is guarded by a
 * seqlock; this has no writer limit and no seqlock, only a relaxed fetch_add.
 * Sharing a cache line between them would also drag every ingress thread's
 * increment into the publication line.
 * ===========================================================================
 */

typedef struct {
    _Alignas(ADCE_CACHELINE) _Atomic uint64_t arrivals;
    uint8_t _reserved[ADCE_CACHELINE - sizeof(_Atomic uint64_t)];
} adce_obs_counter_t;

_Static_assert(sizeof(adce_obs_counter_t) == ADCE_CACHELINE,
               "adce_obs_counter_t must occupy exactly one cache line");
_Static_assert(_Alignof(adce_obs_counter_t) == ADCE_CACHELINE,
               "adce_obs_counter_t must be cache-line aligned");

/* The whole tap. Runs per arrival on the ingress thread, strictly before the
 * enforcement gate, and counts arrivals the gate is about to drop -- tapping
 * after the gate would close a feedback loop in which the detector measures
 * its own output. Relaxed because the count carries no ordering: it is a
 * statistic, read once per epoch by the observer thread. */
static inline void adce_obs_tap(adce_obs_counter_t *counter) {
    atomic_fetch_add_explicit(&counter->arrivals, (uint64_t)1,
                              memory_order_relaxed);
}

/* Snapshot-and-reset, called once per epoch by the observer thread. An
 * exchange rather than a load followed by a store: between those two the
 * ingress threads keep incrementing, and every one of those arrivals would be
 * dropped from the statistic. */
static inline uint64_t adce_obs_counter_take(adce_obs_counter_t *counter) {
    return atomic_exchange_explicit(&counter->arrivals, (uint64_t)0,
                                    memory_order_relaxed);
}

/* ===========================================================================
 * Pure functions on the publication path. Inline and out of the .c so the
 * tests can reach them without a running observer.
 * ===========================================================================
 */

/* Piecewise-linear squash, not logistic: exactly representable in Q16, no
 * transcendental on the path, and saturation points that are auditable
 * numbers rather than curve parameters. */
static inline double adce_obs_squash(double z) {
    if (z <= ADCE_OBS_Z_LO) {
        return 0.0;
    }
    if (z >= ADCE_OBS_Z_HI) {
        return 1.0;
    }
    if (z > ADCE_OBS_Z_LO && z < ADCE_OBS_Z_HI) {
        return (z - ADCE_OBS_Z_LO) / (ADCE_OBS_Z_HI - ADCE_OBS_Z_LO);
    }

    /* Unordered, so z is NaN: it compared false against every bound above.
     * Fail closed. A NaN reaching the Q16 conversion is undefined behaviour,
     * and a statistic that has broken must read as maximal pressure rather
     * than as calm. */
    return 1.0;
}

/* The publication clamp. Applied at publish AND re-applied at read: the value
 * crosses a seqlock and a plane boundary in between, and a reader that trusts
 * the writer's clamp has no defence against a corrupted read.
 *
 * Negative clamps to MAXIMAL, never to zero. A negative can only come from a
 * bug, a bad squash, or a torn read; reading "less than no anomaly" as
 * "nothing to do" fails open at exactly the moment the system is known to be
 * misbehaving. This composes with the locked divide-by-zero rule: a collapsed
 * divisor yields ADCE_Q16_MAX, far outside [0,1], which lands here as maximal
 * pressure and stays distinguishable upstream from a legitimate 1.0. */
static inline adce_q16_t adce_obs_pressure_clamp(adce_q16_t pressure) {
    if (pressure < ADCE_PRESSURE_MIN) {
        return ADCE_PRESSURE_MAX;
    }
    if (pressure > ADCE_PRESSURE_MAX) {
        return ADCE_PRESSURE_MAX;
    }
    return pressure;
}

/* ===========================================================================
 * Observer context. Single-writer by construction and by enforcement.
 * ===========================================================================
 */

typedef struct {
    /* The Observation Plane's internal arithmetic. double by design, owned by
     * the observer thread alone, never shared and never atomic. The Q16
     * conversion at the publish call is the plane boundary; nothing in these
     * three fields ever leaves in floating point. */
    double mu;
    double var;
    double last_z;

    uint64_t epochs_closed;
    uint64_t publications;

    /* Diagnostic only, and deliberately not part of adce_epoch_state_t.
     * Overshoot magnitude is precisely what the hard clamp discards; routing
     * it forward to Enforcement would hand back the unbounded advice the
     * clamp exists to prevent. */
    uint64_t clamp_events;
    adce_q16_t max_overshoot_q16;

    adce_obs_counter_t *counter;
    adce_epoch_state_t *epoch;

    /* Single-writer ownership, enforced rather than assumed. A second
     * claimant fails at startup, loudly, instead of corrupting the seqlock at
     * runtime; pthread_t has no portable null value, so validity gets its own
     * flag rather than a sentinel comparison. */
    _Atomic int writer_claimed;
    pthread_t owner;
    int owner_valid;
} adce_obs_ctx_t;

/* Prepares the context and zeroes the counter. Does not claim the writer:
 * claiming records the calling thread as the owner, and init may legitimately
 * run on a different thread from the observer loop. */
void adce_obs_init(adce_obs_ctx_t *ctx, adce_obs_counter_t *counter,
                   adce_epoch_state_t *epoch);

/* CAS 0 -> 1 on the claim flag, recording the calling thread as owner.
 * Returns 1 to the winner and 0 to every later claimant. */
int adce_obs_claim_writer(adce_obs_ctx_t *ctx);

/* Closes epoch k: snapshot-and-reset the counter, advance the EWMA, squash,
 * and publish.
 *
 * observed_at_ns is a PARAMETER and this function never calls adce_now_ns().
 * The clock is not injectable, so a function that read it internally would
 * make every property this plane claims untestable without a running system.
 *
 * Returns 1 when an epoch was published, 0 when the statistics advanced but
 * warmup suppressed publication, and -1 when the caller is not the claimed
 * writer -- in which case nothing is touched at all. The -1 is fail-closed by
 * construction: publishing nothing lets observed_at_ns go stale and the
 * watchdog takes Enforcement to its conservative posture. */
int adce_obs_epoch_close(adce_obs_ctx_t *ctx, uint64_t observed_at_ns);

/* The clamp with its diagnostic side effect. Exposed so the recording path is
 * reachable from a test without driving a whole epoch through it. */
adce_q16_t adce_obs_clamp_record(adce_obs_ctx_t *ctx, adce_q16_t raw);

#if defined(__cplusplus)
}
#endif

#endif /* ADCE_OBSERVE_H */
