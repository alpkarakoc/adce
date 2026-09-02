/* Observation Plane unit tests.
 *
 * The runner and main() live in t_adce_platform.c. Each case here is static,
 * which is what scripts/verify.sh's guard matches on when it derives the
 * expected test set from the source; static also means internal linkage, so
 * each case gets one external forwarder at the bottom of this file for the
 * runner table in the other translation unit to register. */

#include "../include/adce_observe.h"

#include <pthread.h>
#include <stdio.h>

#define ADCE_TEST_ASSERT(cond)                                               \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
            return 1;                                                        \
        }                                                                    \
    } while (0)

/* fabs() is libm, which neither gate links; see the note on adce_obs_sqrt. */
static double obs_abs(double v) {
    return v < 0.0 ? -v : v;
}

static int obs_close(double got, double want, double tol) {
    double scale = obs_abs(want);
    if (scale < 1.0) {
        scale = 1.0;
    }
    return obs_abs(got - want) <= tol * scale;
}

/* The counter and the epoch state are cache-line aligned, so they are given
 * static storage rather than put on the stack. */
static adce_obs_counter_t g_obs_counter;
static adce_epoch_state_t g_obs_epoch;

static void obs_fresh(adce_obs_ctx_t *ctx, adce_obs_counter_t *counter,
                      adce_epoch_state_t *epoch) {
    memset(epoch, 0, sizeof(*epoch));
    adce_obs_init(ctx, counter, epoch);
}

/* =====================================================================
 * 1. The square root the sigma computation depends on.
 *
 * Written for this plane because libm is unreachable from the gates, so it
 * is new code and carries its own coverage rather than being trusted
 * because the EWMA test happens to pass.
 * ===================================================================== */

static int test_obs_sqrt(void) {
    /* Exact powers of four have exact roots and must come back exact, not
     * merely close: an off-by-one in the exponent seeding would still land
     * within tolerance on these but not on the bit. */
    ADCE_TEST_ASSERT(adce_obs_sqrt(1.0) == 1.0);
    ADCE_TEST_ASSERT(adce_obs_sqrt(4.0) == 2.0);
    ADCE_TEST_ASSERT(adce_obs_sqrt(16.0) == 4.0);
    ADCE_TEST_ASSERT(adce_obs_sqrt(0.25) == 0.5);
    ADCE_TEST_ASSERT(adce_obs_sqrt(1048576.0) == 1024.0);

    /* Non-negative guard, and the fail-closed handling of the values var
     * cannot reach but a corrupted state could. */
    ADCE_TEST_ASSERT(adce_obs_sqrt(0.0) == 0.0);
    ADCE_TEST_ASSERT(adce_obs_sqrt(-1.0) == 0.0);

    {
        volatile double zero = 0.0;
        double nan_v = zero / zero;
        ADCE_TEST_ASSERT(adce_obs_sqrt(nan_v) == 0.0);
    }

    /* Verified from the defining relation -- x*x == v -- rather than against
     * another square root, over a range spanning the exponents a rate
     * variance actually occupies. */
    {
        double v = 1e-6;
        int i;
        for (i = 0; i < 40; ++i) {
            double r = adce_obs_sqrt(v);
            ADCE_TEST_ASSERT(obs_close(r * r, v, 1e-15));
            v *= 7.3;
        }
    }

    /* Odd and negative exponents are where the truncating e/2 seed is at its
     * worst; the fixed step count must still converge from there. */
    ADCE_TEST_ASSERT(obs_close(adce_obs_sqrt(2.0), 1.4142135623730951, 1e-15));
    ADCE_TEST_ASSERT(obs_close(adce_obs_sqrt(0.125), 0.35355339059327379,
                               1e-15));

    return 0;
}

/* =====================================================================
 * 2. EWMA update against an independently derived mean and variance.
 * ===================================================================== */

static int test_obs_ewma_update(void) {
    adce_obs_ctx_t ctx;
    const uint64_t n = 10;
    const double a = ADCE_OBS_ALPHA;
    const double r = (double)n / ADCE_OBS_EPOCH_SECONDS;
    double decay = 1.0;
    double want_var = 0.0;
    int k;

    obs_fresh(&ctx, &g_obs_counter, &g_obs_epoch);
    ADCE_TEST_ASSERT(adce_obs_claim_writer(&ctx) == 1);

    /* First epoch. The deviation is taken against the PRIOR mean, which is
     * zero, so mu_1 is exactly alpha * r. Against the updated mean it would
     * be a different number entirely, which is what makes this one assertion
     * the sharpest check that the ordering is right. */
    atomic_store_explicit(&g_obs_counter.arrivals, n, memory_order_relaxed);
    ADCE_TEST_ASSERT(adce_obs_epoch_close(&ctx, 1000) == 0);
    ADCE_TEST_ASSERT(obs_close(ctx.mu, a * r, 1e-15));

    /* And the closed form for a constant rate from a zero start:
     * mu_k = r * (1 - (1-a)^k). Evaluated by accumulating the decay factor
     * directly, so it does not reuse the recurrence under test. The variance
     * is checked against the same recurrence but driven by the closed-form
     * deviation d_k = r * (1-a)^(k-1), which isolates the prior-mean
     * question from the update arithmetic. */
    decay = 1.0 - a;
    want_var = (1.0 - a) * (0.0 + a * r * r);
    ADCE_TEST_ASSERT(obs_close(ctx.var, want_var, 1e-12));

    for (k = 2; k <= 60; ++k) {
        double want_d = r * decay;

        atomic_store_explicit(&g_obs_counter.arrivals, n, memory_order_relaxed);
        ADCE_TEST_ASSERT(adce_obs_epoch_close(&ctx, (uint64_t)k * 1000) == 0);

        want_var = (1.0 - a) * (want_var + a * want_d * want_d);
        decay *= (1.0 - a);

        ADCE_TEST_ASSERT(obs_close(ctx.mu, r * (1.0 - decay), 1e-12));
        ADCE_TEST_ASSERT(obs_close(ctx.var, want_var, 1e-12));
    }

    /* Structural consequence of a constant input: the mean converges on the
     * rate and the variance collapses toward zero. */
    ADCE_TEST_ASSERT(ctx.mu > 0.0 && ctx.mu < r);
    ADCE_TEST_ASSERT(ctx.var >= 0.0);
    ADCE_TEST_ASSERT(ctx.var < r * r);

    /* Steady traffic drove sigma toward zero, and the epsilon floor is what
     * stops the next ordinary deviation from reading as unbounded z. */
    ADCE_TEST_ASSERT(obs_abs(ctx.last_z) <
                     r / ADCE_OBS_SIGMA_EPSILON + 1.0);

    return 0;
}

/* =====================================================================
 * 3. Squash: monotone, saturating, and bounded.
 * ===================================================================== */

static int test_obs_squash(void) {
    double prev;
    int i;

    ADCE_TEST_ASSERT(adce_obs_squash(ADCE_OBS_Z_LO) == 0.0);
    ADCE_TEST_ASSERT(adce_obs_squash(ADCE_OBS_Z_HI) == 1.0);

    /* Below z_lo is quiet and above z_hi is fully saturated, including the
     * extremes a broken statistic could produce. */
    ADCE_TEST_ASSERT(adce_obs_squash(0.0) == 0.0);
    ADCE_TEST_ASSERT(adce_obs_squash(-1e9) == 0.0);
    ADCE_TEST_ASSERT(adce_obs_squash(ADCE_OBS_Z_LO - 1e-9) == 0.0);
    ADCE_TEST_ASSERT(adce_obs_squash(ADCE_OBS_Z_HI + 1e-9) == 1.0);
    ADCE_TEST_ASSERT(adce_obs_squash(1e9) == 1.0);

    /* The midpoint of a piecewise-linear ramp is exactly one half. */
    ADCE_TEST_ASSERT(adce_obs_squash((ADCE_OBS_Z_LO + ADCE_OBS_Z_HI) / 2.0) ==
                     0.5);

    /* Monotone and never escaping [0,1] across the whole ramp and past both
     * saturation points. */
    prev = -1.0;
    for (i = -200; i <= 1400; ++i) {
        double z = (double)i / 100.0;
        double s = adce_obs_squash(z);

        ADCE_TEST_ASSERT(s >= 0.0 && s <= 1.0);
        ADCE_TEST_ASSERT(s >= prev);
        prev = s;
    }
    ADCE_TEST_ASSERT(prev == 1.0);

    /* NaN compares false against every bound, so it must land on maximal
     * pressure rather than falling through the ramp: the Q16 conversion of a
     * NaN is undefined, and a broken statistic must not read as calm. */
    {
        volatile double zero = 0.0;
        double nan_v = zero / zero;
        ADCE_TEST_ASSERT(adce_obs_squash(nan_v) == 1.0);
    }

    return 0;
}

/* =====================================================================
 * 4. Publication clamp, including the divide-by-zero composition.
 * ===================================================================== */

static int test_obs_publication_clamp(void) {
    adce_obs_ctx_t ctx;

    /* In-range values pass through untouched. */
    ADCE_TEST_ASSERT(adce_obs_pressure_clamp(0) == 0);
    ADCE_TEST_ASSERT(adce_obs_pressure_clamp(ADCE_PRESSURE_MAX) ==
                     ADCE_PRESSURE_MAX);
    ADCE_TEST_ASSERT(adce_obs_pressure_clamp(ADCE_Q16_ONE / 2) ==
                     ADCE_Q16_ONE / 2);

    /* Overshoot saturates rather than wrapping. */
    ADCE_TEST_ASSERT(adce_obs_pressure_clamp(ADCE_PRESSURE_MAX + 1) ==
                     ADCE_PRESSURE_MAX);
    ADCE_TEST_ASSERT(adce_obs_pressure_clamp(ADCE_Q16_MAX) ==
                     ADCE_PRESSURE_MAX);

    /* Negative clamps to MAXIMAL, never to zero. */
    ADCE_TEST_ASSERT(adce_obs_pressure_clamp(-1) == ADCE_PRESSURE_MAX);
    ADCE_TEST_ASSERT(adce_obs_pressure_clamp(-ADCE_Q16_ONE) ==
                     ADCE_PRESSURE_MAX);
    ADCE_TEST_ASSERT(adce_obs_pressure_clamp(ADCE_Q16_MIN) ==
                     ADCE_PRESSURE_MAX);

    /* The composition the locked divide-by-zero rule was written for: a
     * collapsed divisor yields ADCE_Q16_MAX, which lands on maximal pressure
     * rather than on zero. Both signs of numerator, since the rule saturates
     * toward the numerator's sign and one of those is negative. */
    ADCE_TEST_ASSERT(adce_obs_pressure_clamp(adce_q16_div(ADCE_Q16_ONE, 0)) ==
                     ADCE_PRESSURE_MAX);
    ADCE_TEST_ASSERT(adce_obs_pressure_clamp(adce_q16_div(-ADCE_Q16_ONE, 0)) ==
                     ADCE_PRESSURE_MAX);
    ADCE_TEST_ASSERT(adce_obs_pressure_clamp(adce_q16_div(0, 0)) ==
                     ADCE_PRESSURE_MAX);

    /* Nothing escapes the contract range, swept across the boundaries. */
    {
        adce_q16_t v;
        for (v = -8; v <= ADCE_PRESSURE_MAX + 8; ++v) {
            adce_q16_t c = adce_obs_pressure_clamp(v);
            ADCE_TEST_ASSERT(c >= ADCE_PRESSURE_MIN && c <= ADCE_PRESSURE_MAX);
        }
    }

    /* Overshoot magnitude is recorded in the diagnostic counter, which lives
     * on the context and never enters adce_epoch_state_t. */
    obs_fresh(&ctx, &g_obs_counter, &g_obs_epoch);
    ADCE_TEST_ASSERT(ctx.clamp_events == 0);
    ADCE_TEST_ASSERT(ctx.max_overshoot_q16 == 0);

    ADCE_TEST_ASSERT(adce_obs_clamp_record(&ctx, ADCE_Q16_ONE / 4) ==
                     ADCE_Q16_ONE / 4);
    ADCE_TEST_ASSERT(ctx.clamp_events == 0);

    ADCE_TEST_ASSERT(adce_obs_clamp_record(&ctx, ADCE_PRESSURE_MAX + 5) ==
                     ADCE_PRESSURE_MAX);
    ADCE_TEST_ASSERT(ctx.clamp_events == 1);
    ADCE_TEST_ASSERT(ctx.max_overshoot_q16 == 5);

    /* A smaller later overshoot must not lower the recorded maximum. */
    ADCE_TEST_ASSERT(adce_obs_clamp_record(&ctx, ADCE_PRESSURE_MAX + 2) ==
                     ADCE_PRESSURE_MAX);
    ADCE_TEST_ASSERT(ctx.clamp_events == 2);
    ADCE_TEST_ASSERT(ctx.max_overshoot_q16 == 5);

    /* Negative excursion: clamped to maximal, and its magnitude recorded. */
    ADCE_TEST_ASSERT(adce_obs_clamp_record(&ctx, -100) == ADCE_PRESSURE_MAX);
    ADCE_TEST_ASSERT(ctx.clamp_events == 3);
    ADCE_TEST_ASSERT(ctx.max_overshoot_q16 == 100);

    /* ADCE_Q16_MIN is the one input whose magnitude is not representable;
     * negating it would be signed overflow, so it reports the largest
     * magnitude there is instead of wrapping back to itself. */
    ADCE_TEST_ASSERT(adce_obs_clamp_record(&ctx, ADCE_Q16_MIN) ==
                     ADCE_PRESSURE_MAX);
    ADCE_TEST_ASSERT(ctx.clamp_events == 4);
    ADCE_TEST_ASSERT(ctx.max_overshoot_q16 == ADCE_Q16_MAX);

    return 0;
}

/* =====================================================================
 * 5. Warmup: nothing is published before N_min epochs.
 * ===================================================================== */

static int test_obs_warmup(void) {
    adce_obs_ctx_t ctx;
    adce_q16_t pressure;
    uint64_t epoch_id;
    uint64_t observed_at_ns;
    uint64_t k;

    obs_fresh(&ctx, &g_obs_counter, &g_obs_epoch);
    ADCE_TEST_ASSERT(adce_obs_claim_writer(&ctx) == 1);

    for (k = 1; k < ADCE_OBS_WARMUP_EPOCHS; ++k) {
        atomic_store_explicit(&g_obs_counter.arrivals, (uint64_t)10,
                              memory_order_relaxed);
        ADCE_TEST_ASSERT(adce_obs_epoch_close(&ctx, k * ADCE_OBS_EPOCH_NS) == 0);
    }

    ADCE_TEST_ASSERT(ctx.publications == 0);
    ADCE_TEST_ASSERT(ctx.epochs_closed == ADCE_OBS_WARMUP_EPOCHS - 1);

    /* The epoch state is untouched, which is the entire cold-start
     * mechanism: observed_at_ns is still 0, so the watchdog reads stale and
     * Enforcement stays conservative. No second mechanism is needed, and the
     * zero pressure sitting in the struct is never what makes this safe. */
    ADCE_TEST_ASSERT(adce_epoch_read(&g_obs_epoch, &pressure, &epoch_id,
                                     &observed_at_ns) == 1);
    ADCE_TEST_ASSERT(observed_at_ns == 0);
    ADCE_TEST_ASSERT(epoch_id == 0);
    ADCE_TEST_ASSERT(adce_epoch_is_stale(observed_at_ns,
                                         ADCE_OBS_WARMUP_EPOCHS *
                                             ADCE_OBS_EPOCH_NS) == 1);

    /* The N_min'th close is the first publication. */
    atomic_store_explicit(&g_obs_counter.arrivals, (uint64_t)10,
                          memory_order_relaxed);
    ADCE_TEST_ASSERT(adce_obs_epoch_close(
                         &ctx, ADCE_OBS_WARMUP_EPOCHS * ADCE_OBS_EPOCH_NS) == 1);
    ADCE_TEST_ASSERT(ctx.publications == 1);

    ADCE_TEST_ASSERT(adce_epoch_read(&g_obs_epoch, &pressure, &epoch_id,
                                     &observed_at_ns) == 1);
    ADCE_TEST_ASSERT(epoch_id == ADCE_OBS_WARMUP_EPOCHS);
    ADCE_TEST_ASSERT(observed_at_ns ==
                     ADCE_OBS_WARMUP_EPOCHS * ADCE_OBS_EPOCH_NS);
    ADCE_TEST_ASSERT(pressure >= ADCE_PRESSURE_MIN &&
                     pressure <= ADCE_PRESSURE_MAX);

    return 0;
}

/* =====================================================================
 * 6. Writer claim: the second claim fails, and a non-owner cannot publish.
 * ===================================================================== */

static adce_obs_ctx_t g_claim_ctx;
static _Atomic int g_claim_second_result;
static _Atomic int g_claim_publish_result;

static void *claim_intruder(void *arg) {
    (void)arg;

    /* A second claimant must be rejected rather than silently becoming a
     * co-writer of a seqlock that tolerates exactly one. */
    atomic_store_explicit(&g_claim_second_result,
                          adce_obs_claim_writer(&g_claim_ctx),
                          memory_order_release);

    /* And even holding a valid context pointer, a thread that is not the
     * owner must not be able to publish. */
    atomic_store_explicit(&g_claim_publish_result,
                          adce_obs_epoch_close(&g_claim_ctx, 12345),
                          memory_order_release);

    return NULL;
}

static int test_obs_writer_claim(void) {
    pthread_t intruder;
    adce_q16_t pressure;
    uint64_t epoch_id;
    uint64_t observed_at_ns;
    uint64_t arrivals_before;

    obs_fresh(&g_claim_ctx, &g_obs_counter, &g_obs_epoch);

    /* Before any claim, even the initialising thread cannot publish. */
    ADCE_TEST_ASSERT(adce_obs_epoch_close(&g_claim_ctx, 1) == -1);

    ADCE_TEST_ASSERT(adce_obs_claim_writer(&g_claim_ctx) == 1);
    /* Repeated claims from the owner itself fail too: the flag is the
     * invariant, not the identity. */
    ADCE_TEST_ASSERT(adce_obs_claim_writer(&g_claim_ctx) == 0);

    atomic_store_explicit(&g_obs_counter.arrivals, (uint64_t)7,
                          memory_order_relaxed);
    arrivals_before =
        atomic_load_explicit(&g_obs_counter.arrivals, memory_order_relaxed);

    atomic_store_explicit(&g_claim_second_result, 99, memory_order_release);
    atomic_store_explicit(&g_claim_publish_result, 99, memory_order_release);

    ADCE_TEST_ASSERT(pthread_create(&intruder, NULL, claim_intruder, NULL) == 0);
    ADCE_TEST_ASSERT(pthread_join(intruder, NULL) == 0);

    ADCE_TEST_ASSERT(atomic_load_explicit(&g_claim_second_result,
                                          memory_order_acquire) == 0);
    ADCE_TEST_ASSERT(atomic_load_explicit(&g_claim_publish_result,
                                          memory_order_acquire) == -1);

    /* A refused publish must leave the arrival counter and the statistics
     * exactly as it found them, or it quietly steals an epoch's arrivals
     * from the real owner. */
    ADCE_TEST_ASSERT(atomic_load_explicit(&g_obs_counter.arrivals,
                                          memory_order_relaxed) ==
                     arrivals_before);
    ADCE_TEST_ASSERT(g_claim_ctx.epochs_closed == 0);
    ADCE_TEST_ASSERT(g_claim_ctx.publications == 0);

    ADCE_TEST_ASSERT(adce_epoch_read(&g_obs_epoch, &pressure, &epoch_id,
                                     &observed_at_ns) == 1);
    ADCE_TEST_ASSERT(observed_at_ns == 0);

    return 0;
}

/* =====================================================================
 * 7. The cadence invariant.
 * ===================================================================== */

static int test_obs_cadence(void) {
    /* The binding check is the _Static_assert in adce_observe.h; this is its
     * runtime mirror, so the invariant is visible as a named case in the
     * runner rather than only as a build that happened not to fail. */
    _Static_assert(ADCE_OBS_EPOCH_NS <= ADCE_ADVICE_TIMEOUT_NS / 2,
                   "epoch period T must not exceed half the advice timeout");

    ADCE_TEST_ASSERT(ADCE_OBS_EPOCH_NS <= ADCE_ADVICE_TIMEOUT_NS / 2);

    /* What the margin buys, asserted through the watchdog itself rather than
     * by restating arithmetic about it. Publication at t=0; each subsequent
     * epoch that fails to publish ages the state by one T. Four consecutive
     * misses leave it at 4T = 40 ms and still fresh. The watchdog compares
     * strictly greater, so even exactly 5T = 50 ms is not yet stale; the
     * fifth missed epoch is the boundary and anything past it trips. */
    ADCE_TEST_ASSERT(adce_epoch_is_stale(0, ADCE_OBS_EPOCH_NS * 4) == 0);
    ADCE_TEST_ASSERT(adce_epoch_is_stale(0, ADCE_OBS_EPOCH_NS * 5) == 0);
    ADCE_TEST_ASSERT(adce_epoch_is_stale(0, ADCE_OBS_EPOCH_NS * 5 + 1) == 1);

    ADCE_TEST_ASSERT(ADCE_OBS_EPOCH_NS == 10ULL * 1000ULL * 1000ULL);
    ADCE_TEST_ASSERT(ADCE_OBS_WINDOW_N == 100);
    ADCE_TEST_ASSERT(ADCE_OBS_Z_LO == 3.0);
    ADCE_TEST_ASSERT(ADCE_OBS_Z_HI == 8.0);

    /* alpha = 2/(N+1), and the epsilon floor is derived from the minimum
     * expressible rate rather than chosen. */
    ADCE_TEST_ASSERT(obs_close(ADCE_OBS_ALPHA, 2.0 / 101.0, 1e-15));
    ADCE_TEST_ASSERT(obs_close(ADCE_OBS_SIGMA_EPSILON,
                               1.0 / ADCE_OBS_EPOCH_SECONDS, 1e-15));

    return 0;
}

/* =====================================================================
 * 8. Determinism: identical input, bit-identical pressure sequence.
 * ===================================================================== */

#define ADCE_OBS_SEQ_EPOCHS (ADCE_OBS_WINDOW_N + 60)

static adce_obs_counter_t g_det_counter;
static adce_epoch_state_t g_det_epoch;

/* A fixed sequence with a burst placed after warmup, so the run crosses the
 * squash ramp instead of sitting at one saturated end where every
 * implementation would agree. */
static uint64_t obs_seq_arrivals(int k) {
    if (k >= ADCE_OBS_WINDOW_N + 10 && k < ADCE_OBS_WINDOW_N + 30) {
        return (uint64_t)(40 + (k % 11) * 9);
    }
    return (uint64_t)(10 + (k % 3));
}

static int obs_run_sequence(adce_q16_t *out, uint64_t *published,
                            double *final_mu, double *final_var) {
    adce_obs_ctx_t ctx;
    int k;

    obs_fresh(&ctx, &g_det_counter, &g_det_epoch);
    ADCE_TEST_ASSERT(adce_obs_claim_writer(&ctx) == 1);

    *published = 0;

    for (k = 1; k <= ADCE_OBS_SEQ_EPOCHS; ++k) {
        /* Both operands cast to uint64_t. uint64_t is unsigned long on both
         * Linux targets while ADCE_OBS_EPOCH_NS is unsigned long long, and
         * GCC's -Wsign-conversion fires on the promotion between them; Clang
         * on the host does not, which is why this only surfaced on the
         * shipping-target gate. */
        uint64_t at_ns = (uint64_t)k * (uint64_t)ADCE_OBS_EPOCH_NS;

        atomic_store_explicit(&g_det_counter.arrivals, obs_seq_arrivals(k),
                              memory_order_relaxed);

        if (adce_obs_epoch_close(&ctx, at_ns) == 1) {
            adce_q16_t pressure = 0;
            uint64_t epoch_id = 0;
            uint64_t observed_at_ns = 0;

            /* Checked, not discarded. The read is single-threaded here so it
             * cannot fail, but a discarded result would leave `pressure`
             * genuinely undefined on the path GCC flagged, and the whole
             * point of this case is that the values it collects are real. */
            ADCE_TEST_ASSERT(adce_epoch_read(&g_det_epoch, &pressure,
                                             &epoch_id, &observed_at_ns) == 1);
            ADCE_TEST_ASSERT(observed_at_ns == at_ns);

            out[*published] = pressure;
            (*published)++;
        }
    }

    *final_mu = ctx.mu;
    *final_var = ctx.var;

    return 0;
}

static int test_obs_determinism(void) {
    static adce_q16_t first[ADCE_OBS_SEQ_EPOCHS];
    static adce_q16_t second[ADCE_OBS_SEQ_EPOCHS];
    uint64_t n_first = 0;
    uint64_t n_second = 0;
    double mu_first = 0.0;
    double mu_second = 0.0;
    double var_first = 0.0;
    double var_second = 0.0;
    uint64_t i;
    int saw_intermediate = 0;

    ADCE_TEST_ASSERT(obs_run_sequence(first, &n_first, &mu_first,
                                      &var_first) == 0);
    ADCE_TEST_ASSERT(obs_run_sequence(second, &n_second, &mu_second,
                                      &var_second) == 0);

    ADCE_TEST_ASSERT(n_first == n_second);
    ADCE_TEST_ASSERT(n_first == ADCE_OBS_SEQ_EPOCHS - ADCE_OBS_WINDOW_N + 1);

    /* Bit-identical, not merely close. Determinism is what makes every other
     * assertion in this file mean anything: a pressure sequence that drifts
     * between runs would make a passing threshold test evidence of nothing. */
    for (i = 0; i < n_first; ++i) {
        ADCE_TEST_ASSERT(first[i] == second[i]);
        ADCE_TEST_ASSERT(first[i] >= ADCE_PRESSURE_MIN &&
                         first[i] <= ADCE_PRESSURE_MAX);
        if (first[i] > ADCE_PRESSURE_MIN && first[i] < ADCE_PRESSURE_MAX) {
            saw_intermediate = 1;
        }
    }

    /* The sequence has to actually exercise the ramp, or "identical" would be
     * a statement about two runs of constant zero. */
    ADCE_TEST_ASSERT(saw_intermediate == 1);

    /* The double statistics behind the conversion are bit-identical too,
     * compared as object representations so the check cannot be softened by
     * a floating-point comparison. */
    ADCE_TEST_ASSERT(memcmp(&mu_first, &mu_second, sizeof(mu_first)) == 0);
    ADCE_TEST_ASSERT(memcmp(&var_first, &var_second, sizeof(var_first)) == 0);

    return 0;
}

/* =====================================================================
 * 9. The many-writer tap against the single-writer epoch state.
 *
 * The design doc and the step brief both name conflating these two objects
 * as the most likely defect here, and nothing above covers it: every case so
 * far drives the counter from one thread.
 * ===================================================================== */

#define ADCE_OBS_TAP_THREADS 4
#define ADCE_OBS_TAP_PER_THREAD 50000

static adce_obs_counter_t g_tap_counter;

static void *tap_worker(void *arg) {
    int i;
    (void)arg;

    for (i = 0; i < ADCE_OBS_TAP_PER_THREAD; ++i) {
        adce_obs_tap(&g_tap_counter);
    }

    return NULL;
}

static int test_obs_tap_counter(void) {
    pthread_t workers[ADCE_OBS_TAP_THREADS];
    int i;
    uint64_t taken;

    atomic_store_explicit(&g_tap_counter.arrivals, (uint64_t)0,
                          memory_order_relaxed);

    for (i = 0; i < ADCE_OBS_TAP_THREADS; ++i) {
        ADCE_TEST_ASSERT(pthread_create(&workers[i], NULL, tap_worker, NULL) ==
                         0);
    }
    for (i = 0; i < ADCE_OBS_TAP_THREADS; ++i) {
        ADCE_TEST_ASSERT(pthread_join(workers[i], NULL) == 0);
    }

    /* Not one arrival lost across four concurrent writers. A non-atomic or
     * read-modify-write-split increment loses counts here; TSan fails the
     * third profile on it outright. */
    taken = adce_obs_counter_take(&g_tap_counter);
    ADCE_TEST_ASSERT(taken == (uint64_t)ADCE_OBS_TAP_THREADS *
                                  (uint64_t)ADCE_OBS_TAP_PER_THREAD);

    /* Snapshot-and-reset, so the next epoch starts from zero rather than
     * re-counting the last one. */
    ADCE_TEST_ASSERT(adce_obs_counter_take(&g_tap_counter) == 0);

    /* The two objects are distinct and cache-line separated, which is what
     * keeps the ingress threads' increments out of the publication line. */
    ADCE_TEST_ASSERT(sizeof(adce_obs_counter_t) == ADCE_CACHELINE);
    ADCE_TEST_ASSERT((const void *)&g_tap_counter !=
                     (const void *)&g_obs_epoch);
    ADCE_TEST_ASSERT(atomic_is_lock_free(&g_tap_counter.arrivals));

    return 0;
}

/* =====================================================================
 * External forwarders. The cases above are static so the gate's ran-tests
 * guard can find them by source pattern; the runner table lives in
 * t_adce_platform.c, another translation unit, which internal linkage puts
 * out of reach. One forwarder per case is what bridges the two.
 * ===================================================================== */

#define ADCE_OBS_TEST_EXPORT(name)                                           \
    int adce_t_##name(void);                                                 \
    int adce_t_##name(void) { return test_##name(); }

ADCE_OBS_TEST_EXPORT(obs_sqrt)
ADCE_OBS_TEST_EXPORT(obs_ewma_update)
ADCE_OBS_TEST_EXPORT(obs_squash)
ADCE_OBS_TEST_EXPORT(obs_publication_clamp)
ADCE_OBS_TEST_EXPORT(obs_warmup)
ADCE_OBS_TEST_EXPORT(obs_writer_claim)
ADCE_OBS_TEST_EXPORT(obs_cadence)
ADCE_OBS_TEST_EXPORT(obs_determinism)
ADCE_OBS_TEST_EXPORT(obs_tap_counter)
