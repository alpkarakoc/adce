#ifndef ADCE_ENFORCE_H
#define ADCE_ENFORCE_H

/* adce_observe.h owns the publication contract -- ADCE_PRESSURE_MIN/MAX and
 * adce_obs_pressure_clamp -- and pulls in adce_platform.h ahead of any libc
 * header. Enforcement consumes that contract; it does not redefine it, because
 * a consumer with its own private copy of the clamp is a consumer that can
 * disagree with the producer about what the range is. */
#include "adce_observe.h"

#if defined(__cplusplus)
extern "C" {
#endif

/* ===========================================================================
 * Enforcement Plane. Reads what the Observation Plane publishes and decides
 * drop or admit, per arrival, on the ingress thread.
 *
 * Integer arithmetic only. adce_rng_next_unit() and every floating-point
 * operation are confined to the Observation Plane by the convention locked at
 * adce_platform.h:381-387; nothing in this header computes in double.
 *
 * Concurrency contract: adce_epoch_state_t is the only shared object and this
 * plane only ever READS it. Everything this plane writes lives in a
 * per-thread adce_enf_ctx_t. No mutex, no spinlock, no allocation.
 * ===========================================================================
 */

typedef enum {
    ADCE_ENF_ADMIT = 0,
    ADCE_ENF_DROP_SHED = 1,  /* stage one: proportional stochastic shedding */
    ADCE_ENF_DROP_LIMIT = 2  /* stage two: absolute token-bucket ceiling */
} adce_enf_outcome_t;

/* Substituted for the published pressure whenever the watchdog reads stale.
 *
 * Neither extreme is acceptable and the cold start is what decides it. Total
 * drop turns a detector fault into an outage -- and because warmup suppresses
 * publication for ADCE_OBS_WARMUP_EPOCHS, a maximal fallback would mean a full
 * one-second blackout at every process start, deploy and scale-out. Total
 * admit is the precise failure the watchdog exists to prevent, and would make
 * stalling the observer thread the cheapest attack on the system.
 *
 * Half sheds half the traffic and leaves the blind window narrowed rather than
 * wide open. What makes a non-maximal value safe is that stage two does not
 * consult the epoch state at all, so the absolute ceiling keeps holding
 * throughout the stale window regardless of the detector's health. */
#define ADCE_ENF_STALE_PRESSURE (ADCE_Q16_ONE / 2)

_Static_assert(ADCE_ENF_STALE_PRESSURE > ADCE_PRESSURE_MIN &&
                   ADCE_ENF_STALE_PRESSURE < ADCE_PRESSURE_MAX,
               "the stale posture must be neither total admit nor total drop");

/* --- deployment tuning, NOT design constants -------------------------------
 *
 * These three are the only numbers here a deployment is expected to change,
 * and they are per-thread rather than global: each ingress thread carries its
 * own bucket, so the aggregate admitted ceiling is threads * rate. Naming them
 * for the per-thread quantity is deliberate -- reading ADCE_ENF_RATE as a
 * global ceiling and being surprised by N times it is the failure this comment
 * exists to prevent.
 *
 * The bucket accumulates whole Q16 units per nanosecond, so the granularity of
 * the rate knob is 1e9 / 65536 ~ 15,259 admissions per second. 7 units/ns is
 * ~106,800/s per thread. A deployment needing finer resolution should scale
 * ADCE_ENF_COST_Q16 up rather than try to express a fractional rate, which
 * would truncate to zero and close the gate entirely.
 *
 * No test asserts these values. The tests assert that the ceiling HOLDS, so
 * retuning them cannot make a test lie about the property being protected. */
#define ADCE_ENF_COST_Q16 ((uint64_t)ADCE_Q16_ONE)
#define ADCE_ENF_RATE_Q16_PER_NS ((uint64_t)7)
#define ADCE_ENF_CAPACITY_Q16 ((uint64_t)ADCE_Q16_ONE * 4096)

_Static_assert(ADCE_ENF_RATE_Q16_PER_NS > 0,
               "a zero refill rate closes the gate permanently once the "
               "initial burst is spent");
_Static_assert(ADCE_ENF_CAPACITY_Q16 >= ADCE_ENF_COST_Q16,
               "a bucket that cannot hold one arrival's cost never admits");

/* Per-thread. Never shared, so none of these fields is atomic. */
typedef struct {
    /* Shared and read-only to this plane. const is the type system carrying
     * the single-writer invariant the seqlock depends on. */
    const adce_epoch_state_t *epoch;

    uint64_t tokens_q16;
    uint64_t last_refill_ns;

    /* Telemetry. Counting outcomes is permitted; feeding them back into the
     * arrival count or the EWMA is not. That would close the actuation loop by
     * another route and recreate the self-soothing failure the tap placement
     * exists to prevent -- see docs/observation-plane.md section 2. */
    uint64_t admitted;
    uint64_t dropped_shed;
    uint64_t dropped_limit;
    uint64_t stale_reads;
} adce_enf_ctx_t;

/* ===========================================================================
 * Stage one: proportional stochastic shedding.
 * ===========================================================================
 */

/* Returns non-zero when this arrival should be shed.
 *
 * The draw is a PARAMETER, not drawn internally, and that is structural rather
 * than stylistic: adce_rng_tls is `static _Thread_local` at file scope, so each
 * translation unit holds its own stream. A test TU cannot observe or seed the
 * stream an enforcement TU would draw from, so a function that drew internally
 * would be untestable in principle, not merely inconvenient. Same reasoning as
 * observed_at_ns in adce_obs_epoch_close.
 *
 * The shift is the whole mapping and it is exact: draw >> 48 partitions the
 * 64-bit output space into 65536 buckets of 2^48 values each, so the shed
 * fraction is pressure/65536 with no modulo bias at any pressure. Both
 * endpoints land exactly -- pressure 0 sheds nothing, ADCE_PRESSURE_MAX sheds
 * everything -- rather than being off by one bucket.
 *
 * The HIGH bits, deliberately. xorshift128+ is weakest in its low bits; its
 * lowest is a bare LFSR sequence. Taking draw & 0xFFFF would produce the same
 * shed FRACTION from a uniform stream, so no test of the fraction would catch
 * it -- only the independence between successive decisions would be wrong.
 *
 * Clamping here as well as at the read makes the function total over the whole
 * adce_q16_t domain, so no caller can reach the comparison with a value
 * outside the contract. */
static inline int adce_enf_should_shed(adce_q16_t pressure_q16, uint64_t draw) {
    return (draw >> 48) < (uint64_t)adce_obs_pressure_clamp(pressure_q16);
}

/* ===========================================================================
 * The gate.
 * ===========================================================================
 */

/* The full decision, with the draw injected. Deterministic in
 * (epoch contents, now_ns, draw), which is what makes every property in
 * docs/enforcement-plane.md section 5 unit-testable. */
static inline adce_enf_outcome_t adce_enf_decide(adce_enf_ctx_t *ctx,
                                                 uint64_t now_ns,
                                                 uint64_t draw) {
    adce_q16_t pressure = 0;
    uint64_t epoch_id = 0;
    uint64_t observed_at_ns = 0;
    uint64_t elapsed_ns;
    int have_snapshot;

    have_snapshot =
        adce_epoch_read(ctx->epoch, &pressure, &epoch_id, &observed_at_ns);

    /* Recomputed from a fresh now_ns on EVERY arrival, even where a caller
     * caches the snapshot. Staleness is a function of the current time, not of
     * the snapshot alone, so a cached VERDICT freezes a decision whose entire
     * purpose is to change -- and a frozen "fresh" verdict is a gate that
     * fails open exactly when the observer has died.
     *
     * A torn read is treated as a stale one. Retrying on the arrival path is
     * unbounded work for a value that is about to be replaced anyway, and no
     * snapshot means no advice, which is the conservative reading. */
    if (!have_snapshot || adce_epoch_is_stale(observed_at_ns, now_ns)) {
        pressure = ADCE_ENF_STALE_PRESSURE;
        ctx->stale_reads++;
    } else {
        /* Re-clamped at read, not merely at publish. The value crossed a
         * seqlock and a plane boundary to get here, and a reader that trusts
         * the writer's clamp has no defence against a corrupted read. Negative
         * reads as MAXIMAL, never as zero. */
        pressure = adce_obs_pressure_clamp(pressure);
    }

    if (adce_enf_should_shed(pressure, draw)) {
        ctx->dropped_shed++;
        return ADCE_ENF_DROP_SHED;
    }

    /* Stage two: the absolute ceiling. Its rate is NOT a function of pressure,
     * and that independence is the point. pressure is a z-score against a ~1 s
     * EWMA, so an attacker who ramps slowly raises mu along with the rate; d
     * stays small, z stays under z_lo, and pressure sits at zero while absolute
     * volume climbs without bound. This bucket is the floor of protection under
     * a detector that has been talked out of alarming. Tying its rate to
     * pressure would reopen that path exactly.
     *
     * Shedding ran first so that arrivals already destined to drop never touch
     * this state, keeping writes here proportional to admitted rather than to
     * offered load. */
    elapsed_ns = now_ns > ctx->last_refill_ns ? now_ns - ctx->last_refill_ns : 0;
    ctx->tokens_q16 =
        adce_token_refill(ctx->tokens_q16, ADCE_ENF_RATE_Q16_PER_NS, elapsed_ns,
                          ADCE_ENF_CAPACITY_Q16);
    ctx->last_refill_ns = now_ns;

    if (!adce_token_try_take(&ctx->tokens_q16, ADCE_ENF_COST_Q16)) {
        ctx->dropped_limit++;
        return ADCE_ENF_DROP_LIMIT;
    }

    ctx->admitted++;
    return ADCE_ENF_ADMIT;
}

/* The call an ingress site makes. Supplies the draw from the CALLING
 * translation unit's stream, which is the stream adce_enf_thread_init below
 * warms. */
static inline adce_enf_outcome_t adce_enf_admit(adce_enf_ctx_t *ctx,
                                                uint64_t now_ns) {
    return adce_enf_decide(ctx, now_ns, adce_rng_next());
}

/* Once per ingress thread, at thread start, off the request path.
 *
 * The bucket starts full: a thread that began empty would be throttled for its
 * first burst, making every scale-out event a throttling event -- the same
 * class of self-inflicted problem as a maximal stale fallback. The cost is that
 * N fresh threads can burst N * capacity at once, which is the per-thread
 * ceiling documented above behaving as designed rather than a surprise.
 *
 * The discarded draw is the point of the function, not a side effect.
 * adce_rng_next() seeds lazily and adce_rng_seed() aborts on a failed entropy
 * draw, so without this the FIRST arrival on a new thread can abort the process
 * mid-traffic. Drawing here moves that abort to thread creation, where a
 * supervisor can see it. The seeding decision itself is locked and correct; only
 * its timing is moved.
 *
 * This MUST be inline here rather than out-of-line in a .c file. adce_rng_tls
 * is `static _Thread_local` at file scope, so every translation unit has its
 * own stream: an out-of-line warmer would seed the enforcement TU's stream
 * while the inline adce_enf_admit above draws from the caller's, leaving the
 * abort exactly where this call was meant to move it from -- and doing so
 * silently, since both functions would still appear to work. */
static inline void adce_enf_thread_init(adce_enf_ctx_t *ctx,
                                        const adce_epoch_state_t *epoch,
                                        uint64_t now_ns) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->epoch = epoch;
    ctx->tokens_q16 = ADCE_ENF_CAPACITY_Q16;
    ctx->last_refill_ns = now_ns;

    (void)adce_rng_next();
}

#if defined(__cplusplus)
}
#endif

#endif /* ADCE_ENFORCE_H */
