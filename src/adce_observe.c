/* Observation Plane producer.
 *
 * Everything here runs once per epoch on the single observer thread, off the
 * arrival path. The per-arrival half of this plane is one relaxed fetch_add
 * and lives inline in the header. */

#include "adce_observe.h"

#include <math.h>

/* Ownership test run on every publish. Affordable precisely because publish
 * is once per epoch rather than once per event, which is why it stays always
 * on instead of hiding behind NDEBUG -- a check compiled out of the shipping
 * build is not an enforcement of anything. */
static int obs_is_owner(const adce_obs_ctx_t *ctx) {
    if (atomic_load_explicit(&ctx->writer_claimed, memory_order_acquire) != 2) {
        return 0;
    }
    if (!ctx->owner_valid) {
        return 0;
    }
    return pthread_equal(ctx->owner, pthread_self()) != 0;
}

/* The squash output is already in [0,1], so the product stays inside the Q16
 * lane and the truncating cast is a floor -- the project-wide rounding
 * direction, matching adce_q16_to_int and adce_q16_div. */
static adce_q16_t obs_unit_to_q16(double unit) {
    return (adce_q16_t)(unit * (double)ADCE_PRESSURE_MAX);
}

void adce_obs_init(adce_obs_ctx_t *ctx, adce_obs_counter_t *counter,
                   adce_epoch_state_t *epoch) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->counter = counter;
    ctx->epoch = epoch;

    atomic_store_explicit(&counter->arrivals, (uint64_t)0,
                          memory_order_relaxed);
    atomic_store_explicit(&ctx->writer_claimed, 0, memory_order_release);
}

int adce_obs_claim_writer(adce_obs_ctx_t *ctx) {
    int expected = 0;

    /* Three states, not two, and the third is load-bearing. A plain 0 -> 1
     * CAS would publish the claim before ctx->owner had been written, so a
     * concurrent adce_obs_epoch_close could observe "claimed" and then read an
     * uninitialised owner. Winning the CAS moves to 1 (claim in progress),
     * the owner is recorded, and only then does the release store to 2 make
     * the claim visible -- which the acquire load in obs_is_owner pairs with.
     * A second claimant still fails, because its CAS from 0 sees 1 or 2. */
    if (!atomic_compare_exchange_strong_explicit(&ctx->writer_claimed,
                                                 &expected, 1,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        return 0;
    }

    ctx->owner = pthread_self();
    ctx->owner_valid = 1;
    atomic_store_explicit(&ctx->writer_claimed, 2, memory_order_release);

    return 1;
}

adce_q16_t adce_obs_clamp_record(adce_obs_ctx_t *ctx, adce_q16_t raw) {
    adce_q16_t overshoot = 0;

    if (raw > ADCE_PRESSURE_MAX) {
        overshoot = raw - ADCE_PRESSURE_MAX;
    } else if (raw < ADCE_PRESSURE_MIN) {
        /* Magnitude of the excursion below zero. Negating ADCE_Q16_MIN
         * overflows int64_t, which is undefined and which UBSan fails the
         * second profile on, so that one input reports the largest
         * representable magnitude instead of wrapping back to itself. */
        overshoot = (raw == ADCE_Q16_MIN) ? ADCE_Q16_MAX : -raw;
    }

    if (overshoot != 0) {
        ctx->clamp_events++;
        if (overshoot > ctx->max_overshoot_q16) {
            ctx->max_overshoot_q16 = overshoot;
        }
    }

    return adce_obs_pressure_clamp(raw);
}

int adce_obs_epoch_close(adce_obs_ctx_t *ctx, uint64_t observed_at_ns) {
    uint64_t arrivals;
    double rate;
    double d;
    double sigma;
    adce_q16_t pressure_q16;

    /* Checked before anything is snapshotted or advanced: a wrong-thread call
     * must leave the counter and the statistics exactly as it found them, or
     * it silently steals an epoch's arrivals from the real owner. */
    if (!obs_is_owner(ctx)) {
        return -1;
    }

    arrivals = adce_obs_counter_take(ctx->counter);
    rate = (double)arrivals / ADCE_OBS_EPOCH_SECONDS;

    /* Deviation against the PRIOR mean. Taking it against the updated mean
     * would let the current sample pull the baseline toward itself and mask
     * its own excursion, which is the whole failure this ordering prevents. */
    d = rate - ctx->mu;
    ctx->mu = ctx->mu + ADCE_OBS_ALPHA * d;
    ctx->var = (1.0 - ADCE_OBS_ALPHA) * (ctx->var + ADCE_OBS_ALPHA * d * d);

    /* Epsilon floor on sigma, written as a negated >= rather than as an
     * ordinary <. The two differ on exactly one input -- NaN -- and that
     * difference is the entire reason for the shape: `sigma < EPS` is FALSE
     * for a NaN, so a NaN would pass the floor untouched and make z NaN. The
     * negated form is unordered-safe, so this one comparison covers NaN,
     * negative and zero together.
     *
     * That is defence in depth, not a guard against a reachable input, and
     * the distinction is deliberate. var cannot be NaN or infinite here:
     * arrivals is a uint64_t so rate is finite and non-negative, T is a
     * nonzero compile-time constant so the division yields neither inf nor
     * 0/0, mu stays inside the convex hull of the rates it averages so
     * |d| <= rate_max, and the recurrence is a contraction with fixed point
     * (1-alpha)*max(d^2) ~ 3.3e42 -- 266 orders of magnitude below DBL_MAX,
     * measured at UINT64_MAX arrivals per epoch. The floor has to exist
     * anyway for the reachable case, where steady traffic collapses var
     * toward zero; writing it totally costs nothing.
     *
     * The one non-finite value this does NOT normalise is +inf, which would
     * make z zero and read as calm. It is unreachable by the same bound, and
     * is deliberately left unguarded rather than defended against twice. */
    sigma = sqrt(ctx->var);
    if (!(sigma >= ADCE_OBS_SIGMA_EPSILON)) {
        sigma = ADCE_OBS_SIGMA_EPSILON;
    }
    ctx->last_z = d / sigma;

    ctx->epochs_closed++;

    /* Warmup publishes nothing. The statistics above still advance -- that is
     * what warming means -- but the epoch state is left untouched, so
     * observed_at_ns stays 0, adce_epoch_is_stale() stays true, and
     * Enforcement holds its conservative posture. No second mechanism. */
    if (ctx->epochs_closed < ADCE_OBS_WARMUP_EPOCHS) {
        return 0;
    }

    /* The plane boundary, in one expression: the double statistic becomes a
     * clamped Q16 value here and never travels any further as floating
     * point. */
    pressure_q16 =
        adce_obs_clamp_record(ctx, obs_unit_to_q16(adce_obs_squash(ctx->last_z)));

    adce_epoch_publish(ctx->epoch, pressure_q16, ctx->epochs_closed,
                       observed_at_ns);
    ctx->publications++;

    return 1;
}
