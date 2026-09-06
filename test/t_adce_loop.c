/* Closed-loop harness: the detector and the actuator run against each other
 * over TIME, which no other file here does.
 *
 * docs/closed-loop-harness.md is the design. This file lands cases 1-4 of its
 * section 7 and deliberately stops there. Cases 5 and 6 -- the ramps -- rest on
 * the section 2B fixed-point derivation, and that derivation has never been
 * confronted with the code; every case below is decidable from the offered
 * sequence and the published integers alone, so none of them inherits that
 * exposure.
 *
 * NOTHING HERE ASSERTS A BAND. Section 5 of the design is explicit that a
 * settle band B and a direction-change bound D have no derivation, and that
 * tuning one to observed trajectories would be the fifth occasion in this
 * project of a single observation being quoted as a rate. So the metric cases
 * assert EXACT integer values of PP/TV/DC/R on fabricated sequences, and the
 * rig cases assert bit-identity and bit-divergence. Where a number is only
 * observed, it is printed and not asserted.
 *
 * The runner and main() live in t_adce_platform.c; each case here is static so
 * the ran-tests guard in scripts/verify.sh finds it by source pattern, with one
 * external forwarder at the bottom. */

#include "../include/adce_enforce.h"

#include <math.h>
#include <stdio.h>

#define ADCE_TEST_ASSERT(cond)                                                \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
            return 1;                                                         \
        }                                                                     \
    } while (0)

/* =====================================================================
 * The rig's own draw stream.
 *
 * NOT adce_rng_next(). That draws from adce_rng_tls, which is
 * `static _Thread_local` at file scope and seeded from kernel entropy, so a
 * test TU can neither observe nor seed it -- which is the whole reason
 * adce_enf_decide takes the draw as a parameter in the first place
 * (docs/enforcement-plane.md section 5). A rig whose premise is "same offered
 * sequence, two DIFFERENT draw streams, byte-identical everything else" has to
 * own both streams outright.
 *
 * splitmix64, because the property under test is that the draws are
 * reproducible and that two seeds give different sequences -- not that they
 * are cryptographically anything. It is not a runtime dependency: it is nine
 * lines in a test file and never links into the library.
 * ===================================================================== */

typedef struct {
    uint64_t s;
} loop_draws_t;

static uint64_t loop_draw_next(loop_draws_t *d) {
    uint64_t z = (d->s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* =====================================================================
 * The settle metrics of docs/closed-loop-harness.md section 3.
 *
 *   PP(W) = max(p) - min(p)                  peak-to-peak
 *   TV(W) = sum |p_{i+1} - p_i|              total variation
 *   DC(W) = sign changes in the NONZERO diffs
 *   R(W)  = TV / PP, and 0 when PP == 0
 *
 * All four are pure functions of a sequence of published pressures. No system
 * runs, nothing is timed, and every value below is an exact integer.
 * ===================================================================== */

typedef struct {
    adce_q16_t pp;
    uint64_t tv;
    uint64_t dc;
    adce_q16_t r_q16; /* TV/PP as a Q16 ratio; dimensionless */
} loop_metrics_t;

static loop_metrics_t loop_metrics(const adce_q16_t *p, size_t n) {
    loop_metrics_t m;
    adce_q16_t lo;
    adce_q16_t hi;
    size_t i;
    int prev_sign = 0;

    memset(&m, 0, sizeof(m));
    if (n == 0) {
        return m;
    }

    lo = p[0];
    hi = p[0];

    for (i = 1; i < n; i++) {
        adce_q16_t d = p[i] - p[i - 1];
        int sign;

        if (p[i] < lo) {
            lo = p[i];
        }
        if (p[i] > hi) {
            hi = p[i];
        }

        /* A zero diff carries no direction, so it neither adds to TV nor
         * breaks a run of one sign. Skipping rather than treating it as a
         * third sign is what makes a monotone sequence WITH flat spots still
         * score DC == 0 -- a decay that stalls at its limit is settling, not
         * a direction change. */
        if (d == 0) {
            continue;
        }

        m.tv += (uint64_t)(d < 0 ? -d : d);
        sign = d < 0 ? -1 : 1;
        if (prev_sign != 0 && sign != prev_sign) {
            m.dc++;
        }
        prev_sign = sign;
    }

    m.pp = hi - lo;

    /* R := 0 at PP == 0, written as its own branch and NOT delegated to
     * adce_q16_div. That function's zero-divisor contract is locked and
     * fail-closed -- it saturates toward the numerator's sign, so 0/0 and
     * TV/0 both yield ADCE_Q16_MAX. Correct for a pressure signal, and
     * exactly inverted here: PP == 0 is a perfectly FLAT trajectory, the
     * strongest evidence of settling this metric can produce, and routing it
     * through the divide would report it as maximal oscillation. */
    m.r_q16 = (m.pp == 0) ? 0 : adce_q16_div((adce_q16_t)m.tv, m.pp);

    return m;
}

/* The implied cycle period of section 3, 2W/R epochs, in Q16 epochs. Reported
 * beside R because it is the number a reader can check against the load's own
 * period -- and, in the transient case below, the number that exposes R having
 * been deflated when R itself still looks alarming. */
static adce_q16_t loop_implied_period_q16(const loop_metrics_t *m, size_t n) {
    if (m->tv == 0) {
        return 0;
    }
    /* 2*n*PP/TV, kept in one divide so the only rounding is the Q16 floor. */
    return adce_q16_div((adce_q16_t)(2u * (uint64_t)n) * m->pp,
                        (adce_q16_t)m->tv);
}

/* Permutation-INVARIANT dispersion: W*sum(x^2) - (sum x)^2, which is W^2 times
 * the population variance and is an exact integer. Used to show that variance
 * cannot see the ordering that R and DC are built on -- a cycle and its own
 * sorted permutation share a multiset, so this is equal for them bit for bit
 * while R and DC are not. That is a stronger statement than "the cycle has
 * small variance", and it needs no threshold to make. */
static uint64_t loop_dispersion(const adce_q16_t *p, size_t n) {
    uint64_t sum = 0;
    uint64_t sq = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        sum += (uint64_t)p[i];
        sq += (uint64_t)p[i] * (uint64_t)p[i];
    }
    return (uint64_t)n * sq - sum * sum;
}

static void loop_report(const char *label, const loop_metrics_t *m, size_t n) {
    printf("    LOOP %-22s W=%3llu PP=%6lld TV=%9llu DC=%4llu R=%9.4f"
           " period=%9.3f\n",
           label, (unsigned long long)n, (long long)m->pp,
           (unsigned long long)m->tv, (unsigned long long)m->dc,
           (double)m->r_q16 / (double)ADCE_Q16_ONE,
           (double)loop_implied_period_q16(m, n) / (double)ADCE_Q16_ONE);
}

/* =====================================================================
 * The synthetic rig.
 *
 * One thread, model time it owns outright, and no clock read anywhere. Section
 * 4 of the design lists the four fidelity losses this buys and which existing
 * test already covers each; the point of the rig is that two runs of the same
 * configuration produce bit-identical output, which no real-time arm can.
 * ===================================================================== */

#define LOOP_EPOCHS 400u
#define LOOP_BASE_ARRIVALS 400u
#define LOOP_STEP_EPOCH 200u
#define LOOP_STEP_MULT 8u

/* The trailing window of section 3: W >= 4/alpha ~ 200 epochs, four EWMA time
 * constants, so any cycle driven by the detector's own memory completes several
 * times inside it. This is the ONE window length in this file with a
 * derivation, and it comes from the tuning constant rather than from a
 * trajectory. */
#define LOOP_WINDOW 200u

_Static_assert(LOOP_WINDOW * 2u <= ADCE_OBS_WINDOW_N * 4u,
               "the trailing window must be at most 4/alpha ~ 2N epochs");
_Static_assert(LOOP_EPOCHS >= ADCE_OBS_WARMUP_EPOCHS + LOOP_WINDOW,
               "the run must outlast warmup by at least one trailing window, "
               "or the window samples epochs that never published");

typedef struct {
    adce_obs_counter_t counter;
    adce_epoch_state_t epoch;
    adce_obs_ctx_t obs;
    adce_enf_ctx_t enf;
    loop_draws_t draws;
    uint64_t tapped;
    uint64_t work;

    /* A rolling checksum of the per-arrival VERDICT SEQUENCE, and the
     * non-vacuity witness case 2 actually uses.
     *
     * docs/closed-loop-harness.md section 5 specifies that witness as "the two
     * draw streams must produce different dropped_shed counts". Measured here,
     * that assertion is FLAKY, and the first seed pair tried hit it: seeds
     * 0xA5A5A5A5 and 0x5EED1234 both shed exactly 24075 of 720000 arrivals
     * while their verdicts differed throughout. dropped_shed is a scalar sum
     * of ~38000 Bernoulli trials, so two streams land on the same total
     * whenever their sums happen to coincide -- over 24 seeds the totals ran
     * 23801..24282 with a standard deviation of 111, which puts the collision
     * probability near 1/(2*sd*sqrt(pi)) ~ 0.25%. That figure is ANALYTIC,
     * from the observed spread; the single collision is one observation and is
     * deliberately not quoted as a rate.
     *
     * The checksum has no such failure mode. It differs unless the two streams
     * produced the SAME verdict for every one of the 720000 arrivals, which is
     * the exact statement "the draws never mattered" that non-vacuity is
     * meant to exclude. The totals are still printed, because the magnitude of
     * the shedding is worth seeing -- they are just not what is asserted.
     *
     * Read-only: folding a verdict already computed moves no arrival and
     * changes no decision, so it cannot perturb the tap/gate accounting. Same
     * argument as the harness's per-site snapshot, and the opposite of a drain
     * like g_st_thaw_discarded, which did need a term on both sides. */
    uint64_t verdicts;
} loop_rig_t;

/* The LCG constant t_adce_harness.c's work_checksum already uses. Unsigned, so
 * the wraparound is defined and UBSan stays quiet on it. */
static void loop_fold_verdict(loop_rig_t *rig, adce_enf_outcome_t v) {
    rig->verdicts =
        rig->verdicts * 6364136223846793005ULL + (uint64_t)v + 1u;
}

/* Offered arrivals in model epoch k: a step, which section 2A picks because it
 * is the pattern that leaves the linear region of the squash and exercises
 * saturation, the clamp and the recovery path. The offered sequence is a
 * function of k ALONE -- never of a verdict, a draw or a counter -- which is
 * what makes the draw-invariance assertion exact rather than statistical. */
static uint32_t loop_arrivals(uint32_t k) {
    return k < LOOP_STEP_EPOCH ? LOOP_BASE_ARRIVALS
                               : LOOP_BASE_ARRIVALS * LOOP_STEP_MULT;
}

/* THE CORRECT ORDER, and a SEPARATE function from t_adce_harness.c's
 * ingress_correct rather than a reuse of it. Section 4's fourth fidelity note
 * requires exactly that: the shipped recipe calls adce_enf_admit, which draws
 * from the calling TU's real stream, and a rig that injects the draw must call
 * adce_enf_decide instead. adce_enf_admit is a one-line wrapper over
 * adce_enf_decide(ctx, now, adce_rng_next()), so the gate under test is
 * identical and only the draw's provenance differs -- but that is a deviation,
 * and it gets its own function instead of being smuggled into the harness's. */
static void loop_ingress_correct(loop_rig_t *rig, uint64_t now_ns) {
    adce_enf_outcome_t v;

    adce_obs_tap(&rig->counter);
    rig->tapped++;

    v = adce_enf_decide(&rig->enf, now_ns, loop_draw_next(&rig->draws));
    loop_fold_verdict(rig, v);

    if (v != ADCE_ENF_ADMIT) {
        return;
    }
    rig->work++;
}

/* THE INVERTED ORDER: gate first, so the counter records ADMITTED arrivals and
 * the detector measures its own output. This is the internal loop of section 1
 * closed, as executable code. */
static void loop_ingress_inverted(loop_rig_t *rig, uint64_t now_ns) {
    adce_enf_outcome_t v =
        adce_enf_decide(&rig->enf, now_ns, loop_draw_next(&rig->draws));

    loop_fold_verdict(rig, v);

    if (v != ADCE_ENF_ADMIT) {
        return;
    }

    adce_obs_tap(&rig->counter);
    rig->tapped++;
    rig->work++;
}

/* Drives LOOP_EPOCHS model epochs and records the published pressure after
 * each close. Returns 0 on success.
 *
 * Model time is exact: epoch k closes at (k+1)*T regardless of how the
 * arrivals inside it were spaced, so epochs are exactly T apart and the
 * watchdog never trips for a reason the rig invented (section 4, loss 3). */
static int loop_run(loop_rig_t *rig, uint64_t seed,
                    void (*ingress)(loop_rig_t *, uint64_t),
                    adce_q16_t *traj) {
    uint32_t k;

    /* Same memset-the-whole-context discipline as adce_obs_init and
     * adce_enf_thread_init. Legal here for the atomics inside because the rig
     * is single-threaded and nothing has been published yet. */
    memset(rig, 0, sizeof(*rig));
    rig->draws.s = seed;

    adce_obs_init(&rig->obs, &rig->counter, &rig->epoch);
    if (!adce_obs_claim_writer(&rig->obs)) {
        return 1;
    }

    /* The discarded adce_rng_next() inside is inert here -- the rig injects
     * every draw the gate sees, so the TLS stream it warms is never read. It
     * is called anyway rather than hand-rolling the bucket's initial state,
     * because duplicating the library's own thread setup in a test is how the
     * two drift apart. */
    adce_enf_thread_init(&rig->enf, &rig->epoch, 0);

    for (k = 0; k < LOOP_EPOCHS; k++) {
        uint32_t n = loop_arrivals(k);
        uint64_t base_ns = (uint64_t)k * ADCE_OBS_EPOCH_NS;
        uint64_t step_ns = ADCE_OBS_EPOCH_NS / n;
        uint64_t close_ns = (uint64_t)(k + 1u) * ADCE_OBS_EPOCH_NS;
        uint32_t i;
        adce_q16_t pressure = 0;
        uint64_t epoch_id = 0;
        uint64_t observed_at_ns = 0;

        for (i = 0; i < n; i++) {
            ingress(rig, base_ns + (uint64_t)i * step_ns);
        }

        if (adce_obs_epoch_close(&rig->obs, close_ns) < 0) {
            return 1;
        }

        /* Single-threaded, so the seqlock can never straddle and this read
         * cannot tear. Before the first publication it returns the zeroed
         * state, which is the trajectory's true value there: nothing has been
         * published, and warmup publishing nothing is the design. */
        if (!adce_epoch_read(&rig->epoch, &pressure, &epoch_id,
                             &observed_at_ns)) {
            return 1;
        }
        traj[k] = pressure;
    }

    return 0;
}

static int loop_traj_equal(const adce_q16_t *a, const adce_q16_t *b,
                           size_t n, size_t *first_diff) {
    size_t i;

    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            *first_diff = i;
            return 0;
        }
    }
    return 1;
}

/* A trajectory that never moves would satisfy "identical" vacuously, exactly
 * as a rig where pressure is always zero proves nothing about a detector. */
static int loop_traj_is_constant(const adce_q16_t *t, size_t n) {
    size_t i;

    for (i = 1; i < n; i++) {
        if (t[i] != t[0]) {
            return 0;
        }
    }
    return 1;
}

static adce_q16_t loop_traj_max(const adce_q16_t *t, size_t n) {
    adce_q16_t hi = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        if (t[i] > hi) {
            hi = t[i];
        }
    }
    return hi;
}

/* =====================================================================
 * Case 1 -- the rig run twice yields a bit-identical trajectory.
 *
 * Everything below is meaningless if this fails, and it fails loudly if any
 * hidden clock or unseeded stream reached the rig.
 * ===================================================================== */

static int test_loop_synthetic_determinism(void) {
    static adce_q16_t a[LOOP_EPOCHS];
    static adce_q16_t b[LOOP_EPOCHS];
    static loop_rig_t rig;
    uint64_t tapped_a;
    uint64_t shed_a;
    uint64_t admitted_a;
    uint64_t verdicts_a;
    size_t at = 0;

    ADCE_TEST_ASSERT(loop_run(&rig, 1u, loop_ingress_correct, a) == 0);
    tapped_a = rig.tapped;
    shed_a = rig.enf.dropped_shed;
    admitted_a = rig.enf.admitted;
    verdicts_a = rig.verdicts;

    ADCE_TEST_ASSERT(loop_run(&rig, 1u, loop_ingress_correct, b) == 0);

    ADCE_TEST_ASSERT(loop_traj_equal(a, b, LOOP_EPOCHS, &at));
    ADCE_TEST_ASSERT(rig.tapped == tapped_a);
    ADCE_TEST_ASSERT(rig.enf.dropped_shed == shed_a);
    ADCE_TEST_ASSERT(rig.enf.admitted == admitted_a);
    /* Not just the totals: the whole per-arrival verdict sequence repeats. */
    ADCE_TEST_ASSERT(rig.verdicts == verdicts_a);

    /* Not vacuous: the run has to have DONE something. */
    ADCE_TEST_ASSERT(!loop_traj_is_constant(a, LOOP_EPOCHS));
    ADCE_TEST_ASSERT(loop_traj_max(a, LOOP_EPOCHS) > ADCE_PRESSURE_MIN);

    printf("  LOOP determinism tapped=%llu admitted=%llu shed=%llu limit=%llu"
           " publications=%llu max_pressure=%lld\n",
           (unsigned long long)rig.tapped,
           (unsigned long long)rig.enf.admitted,
           (unsigned long long)rig.enf.dropped_shed,
           (unsigned long long)rig.enf.dropped_limit,
           (unsigned long long)rig.obs.publications,
           (long long)loop_traj_max(a, LOOP_EPOCHS));

    return 0;
}

/* =====================================================================
 * Case 2 -- draw-invariance under the correct ordering.
 *
 * The strongest statement in docs/closed-loop-harness.md: with the tap first
 * the counter is a function of the OFFERED sequence alone, so the published
 * pressure trajectory cannot depend on the draws. Exact, epoch for epoch, with
 * no band, threshold or tolerance -- the internal loop is open, over time.
 * ===================================================================== */

static int test_loop_draw_invariance(void) {
    static adce_q16_t a[LOOP_EPOCHS];
    static adce_q16_t b[LOOP_EPOCHS];
    static loop_rig_t rig;
    uint64_t shed_a;
    uint64_t admitted_a;
    uint64_t tapped_a;
    uint64_t verdicts_a;
    size_t at = 0;

    ADCE_TEST_ASSERT(loop_run(&rig, 0xA5A5A5A5u, loop_ingress_correct, a) == 0);
    shed_a = rig.enf.dropped_shed;
    admitted_a = rig.enf.admitted;
    tapped_a = rig.tapped;
    verdicts_a = rig.verdicts;

    ADCE_TEST_ASSERT(loop_run(&rig, 0x5EED1234u, loop_ingress_correct, b) == 0);

    ADCE_TEST_ASSERT(loop_traj_equal(a, b, LOOP_EPOCHS, &at));

    /* The tap counts offered arrivals, so it is draw-independent too, and for
     * the same reason. */
    ADCE_TEST_ASSERT(rig.tapped == tapped_a);
    ADCE_TEST_ASSERT(rig.tapped == LOOP_BASE_ARRIVALS *
                                       (uint64_t)LOOP_STEP_EPOCH +
                                   LOOP_BASE_ARRIVALS * LOOP_STEP_MULT *
                                       (uint64_t)(LOOP_EPOCHS -
                                                  LOOP_STEP_EPOCH));

    /* NON-VACUITY, on the verdict SEQUENCE rather than on a scalar total.
     * Identical trajectories would otherwise be satisfied by draws that never
     * changed a verdict anywhere. See the comment on loop_rig_t::verdicts for
     * why dropped_shed -- the witness section 5 names -- is not sound here:
     * these two seeds collide on it exactly. */
    ADCE_TEST_ASSERT(rig.verdicts != verdicts_a);
    ADCE_TEST_ASSERT(!loop_traj_is_constant(a, LOOP_EPOCHS));

    printf("  LOOP invariance trajectories IDENTICAL over %u epochs |"
           " verdicts DIFFER %016llx vs %016llx | shed %llu vs %llu,"
           " admitted %llu vs %llu\n",
           LOOP_EPOCHS, (unsigned long long)verdicts_a,
           (unsigned long long)rig.verdicts, (unsigned long long)shed_a,
           (unsigned long long)rig.enf.dropped_shed,
           (unsigned long long)admitted_a,
           (unsigned long long)rig.enf.admitted);

    return 0;
}

/* =====================================================================
 * Case 3 -- draw-DEPENDENCE under the inverted ordering. The teeth.
 *
 * If the inverted arm ever stops diverging, case 2 has lost its teeth and its
 * green result means nothing. Passes by OBSERVING a divergence, the way
 * test_harness_tap_after_gate passes by observing a violated identity.
 * ===================================================================== */

static int test_loop_inverted_draw_dependence(void) {
    static adce_q16_t a[LOOP_EPOCHS];
    static adce_q16_t b[LOOP_EPOCHS];
    static loop_rig_t rig;
    uint64_t tapped_a;
    size_t at = 0;
    int identical;

    /* stderr, unbuffered, so this banner cannot be reordered against anything
     * it explains -- the same stream discipline as the harness teeth fence.
     * The divergence printed below is the RESULT, not a fault. */
    fprintf(stderr,
            "  LOOP inverted BEGIN -- a DIVERGENCE below is the expected"
            " outcome;\n"
            "  the inverted ordering closes the internal loop, and case 2's"
            " invariance\n"
            "  assertion is only meaningful because this arm diverges.\n");

    ADCE_TEST_ASSERT(loop_run(&rig, 0xA5A5A5A5u, loop_ingress_inverted, a) == 0);
    tapped_a = rig.tapped;

    ADCE_TEST_ASSERT(loop_run(&rig, 0x5EED1234u, loop_ingress_inverted, b) == 0);

    identical = loop_traj_equal(a, b, LOOP_EPOCHS, &at);
    ADCE_TEST_ASSERT(!identical);

    /* The mechanism, not merely the symptom: with the gate first, the tap
     * counts admitted arrivals, so the DETECTOR'S INPUT itself now depends on
     * the draws. That is the internal loop being closed, and it is what makes
     * the trajectories differ. */
    ADCE_TEST_ASSERT(rig.tapped != tapped_a);
    ADCE_TEST_ASSERT(rig.tapped < LOOP_BASE_ARRIVALS *
                                      (uint64_t)LOOP_STEP_EPOCH +
                                  LOOP_BASE_ARRIVALS * LOOP_STEP_MULT *
                                      (uint64_t)(LOOP_EPOCHS -
                                                 LOOP_STEP_EPOCH));

    fprintf(stderr,
            "  LOOP inverted END -- diverged at epoch %llu (%lld vs %lld),"
            " tapped %llu vs %llu.\n",
            (unsigned long long)at, (long long)a[at], (long long)b[at],
            (unsigned long long)tapped_a, (unsigned long long)rig.tapped);

    return 0;
}

/* =====================================================================
 * Case 4 -- the settle metrics' own teeth, on fabricated sequences.
 *
 * No system runs. Every expected value below is hand-derivable from the
 * sequence, and every assertion is an exact integer identity.
 * ===================================================================== */

#define LOOP_TEETH_W 200u
#define LOOP_TEETH_LONG 400u

/* Fixture builders. Each returns the sequence's length so the cases read as
 * "build, measure, assert" with no index arithmetic at the call site. */

static void loop_fill_square(adce_q16_t *p, size_t n) {
    size_t i;

    for (i = 0; i < n; i++) {
        p[i] = (i & 1u) ? ADCE_PRESSURE_MAX : ADCE_PRESSURE_MIN;
    }
}

/* A geometric decay to a limit, which is what settling after a step looks
 * like. It stalls once the decrement floors to zero; those flat samples are
 * the "approach to a limit" and must not register as direction changes. */
static void loop_fill_decay(adce_q16_t *p, size_t n) {
    size_t i;

    p[0] = ADCE_PRESSURE_MAX;
    for (i = 1; i < n; i++) {
        p[i] = p[i - 1] - (p[i - 1] / 8);
    }
}

/* A slow, SMALL-amplitude square cycle: period 40 epochs, amplitude 200 of
 * 65536 -- 0.3% of full scale. */
#define LOOP_CYCLE_BASE ((adce_q16_t)1000)
#define LOOP_CYCLE_AMP ((adce_q16_t)200)
#define LOOP_CYCLE_HALF 20u

static void loop_fill_slow_cycle(adce_q16_t *p, size_t n) {
    size_t i;

    for (i = 0; i < n; i++) {
        p[i] = LOOP_CYCLE_BASE +
               (((i / LOOP_CYCLE_HALF) & 1u) ? LOOP_CYCLE_AMP : 0);
    }
}

/* The SAME MULTISET, sorted. A permutation, so every moment of the
 * distribution is preserved exactly and only the ORDER differs. */
static void loop_fill_slow_cycle_sorted(adce_q16_t *p, size_t n) {
    size_t i;

    for (i = 0; i < n; i++) {
        p[i] = (i < n / 2) ? LOOP_CYCLE_BASE
                           : (adce_q16_t)(LOOP_CYCLE_BASE + LOOP_CYCLE_AMP);
    }
}

/* A one-time transient followed by a persistent cycle of amplitude `amp`:
 * `prefix` epochs at MIN, then a step to `step`, then alternating forever. */
static void loop_fill_transient_cycle(adce_q16_t *p, size_t n, size_t prefix,
                                      adce_q16_t step, adce_q16_t amp) {
    size_t i;

    for (i = 0; i < prefix; i++) {
        p[i] = ADCE_PRESSURE_MIN;
    }
    for (i = prefix; i < n; i++) {
        p[i] = step + (((i - prefix) & 1u) ? amp : 0);
    }
}

/* Alternation by ONE LSB. Reachable without any fault: pressure is produced by
 * truncating squash(z)*65536 to an integer, one LSB of pressure is
 * (z_hi - z_lo)/65536 = 7.6e-5 in z, and a settled z straddling an integer
 * boundary at that scale is ordinary floating-point behaviour, not a defect. */
static void loop_fill_lsb_dither(adce_q16_t *p, size_t n) {
    size_t i;

    for (i = 0; i < n; i++) {
        p[i] = (adce_q16_t)(ADCE_PRESSURE_MAX / 2) + (adce_q16_t)(i & 1u);
    }
}

static int test_loop_settle_metrics_teeth(void) {
    static adce_q16_t p[LOOP_TEETH_LONG];
    static adce_q16_t q[LOOP_TEETH_LONG];
    loop_metrics_t sq;
    loop_metrics_t dec;
    loop_metrics_t cyc;
    loop_metrics_t srt;
    loop_metrics_t full;
    loop_metrics_t tail;
    loop_metrics_t dither;
    adce_q16_t r_hidden_full;

    printf("  LOOP metric teeth -- fabricated sequences, no system running\n");

    /* --- A. Square wave: the cycle signature, at full scale ------------- */
    loop_fill_square(p, LOOP_TEETH_W);
    sq = loop_metrics(p, LOOP_TEETH_W);
    loop_report("A square period-2", &sq, LOOP_TEETH_W);

    ADCE_TEST_ASSERT(sq.pp == ADCE_PRESSURE_MAX);
    ADCE_TEST_ASSERT(sq.tv == (uint64_t)(LOOP_TEETH_W - 1u) *
                                  (uint64_t)ADCE_PRESSURE_MAX);
    ADCE_TEST_ASSERT(sq.dc == LOOP_TEETH_W - 2u);
    /* R = 199 exactly: 199 traversals of the full range. */
    ADCE_TEST_ASSERT(sq.r_q16 ==
                     (adce_q16_t)(LOOP_TEETH_W - 1u) * ADCE_Q16_ONE);

    /* --- B. Monotone decay: the settling signature --------------------- */
    loop_fill_decay(p, LOOP_TEETH_W);
    dec = loop_metrics(p, LOOP_TEETH_W);
    loop_report("B monotone decay", &dec, LOOP_TEETH_W);

    /* TV == PP exactly: a monotone approach traverses its range once. */
    ADCE_TEST_ASSERT((uint64_t)dec.pp == dec.tv);
    ADCE_TEST_ASSERT(dec.r_q16 == ADCE_Q16_ONE);
    ADCE_TEST_ASSERT(dec.dc == 0);

    /* --- C. THE DETECTION GAP: variance is blind to ordering ----------- *
     * A slow cycle and its own sorted permutation. Same multiset, so the
     * dispersion is equal BIT FOR BIT and no variance-based predicate can
     * separate them at any threshold whatsoever. R and DC separate them
     * exactly. This is what R is for. */
    loop_fill_slow_cycle(p, LOOP_TEETH_W);
    loop_fill_slow_cycle_sorted(q, LOOP_TEETH_W);
    cyc = loop_metrics(p, LOOP_TEETH_W);
    srt = loop_metrics(q, LOOP_TEETH_W);
    loop_report("C slow cycle", &cyc, LOOP_TEETH_W);
    loop_report("C same, sorted", &srt, LOOP_TEETH_W);

    ADCE_TEST_ASSERT(loop_dispersion(p, LOOP_TEETH_W) ==
                     loop_dispersion(q, LOOP_TEETH_W));
    ADCE_TEST_ASSERT(cyc.pp == srt.pp); /* even PP cannot separate them */

    /* 9 block transitions in 200 samples at half-period 20. */
    ADCE_TEST_ASSERT(cyc.tv == 9u * (uint64_t)LOOP_CYCLE_AMP);
    ADCE_TEST_ASSERT(cyc.r_q16 == 9 * ADCE_Q16_ONE);
    ADCE_TEST_ASSERT(cyc.dc == 8);

    ADCE_TEST_ASSERT((uint64_t)srt.pp == srt.tv);
    ADCE_TEST_ASSERT(srt.r_q16 == ADCE_Q16_ONE);
    ADCE_TEST_ASSERT(srt.dc == 0);

    /* R/2 counts the cycles: 4.5 against 5 half-cycles seen in the window. */
    ADCE_TEST_ASSERT(cyc.r_q16 > srt.r_q16);

    /* --- D. R DEFLATION: a transient in the window hides the cycle ----- *
     * Step to 0.8 of full scale, then dither by 0.01 forever. PP is set by
     * the ONE-TIME transient while TV is shared between the transient and the
     * cycle, so R -- a ratio of the two -- is deflated by roughly amp/step. */
    loop_fill_transient_cycle(p, LOOP_TEETH_LONG, 100u, (adce_q16_t)52428,
                              (adce_q16_t)655);
    full = loop_metrics(p, LOOP_TEETH_LONG);
    tail = loop_metrics(p + (LOOP_TEETH_LONG - LOOP_WINDOW), LOOP_WINDOW);
    loop_report("D transient+cycle full", &full, LOOP_TEETH_LONG);
    loop_report("D  same, trailing W", &tail, LOOP_WINDOW);

    ADCE_TEST_ASSERT(full.pp == 53083);
    ADCE_TEST_ASSERT(full.tv == 52428u + 299u * 655u);

    /* The trailing window is CLEAR of the transient by index arithmetic, not
     * by a decay threshold: the step's diff sits at index 100 and the window
     * opens at index 200. What it recovers is exact -- R == 199 and an implied
     * period of 2.01 epochs against a true period of 2. */
    ADCE_TEST_ASSERT(tail.pp == 655);
    ADCE_TEST_ASSERT(tail.tv == (uint64_t)(LOOP_WINDOW - 1u) * 655u);
    ADCE_TEST_ASSERT(tail.r_q16 == (adce_q16_t)(LOOP_WINDOW - 1u) *
                                       ADCE_Q16_ONE);

    /* The deflation is a factor of ~42 on R and ~85 on the reported period.
     * Both windows see the SAME cycle; only one of them reports it. */
    ADCE_TEST_ASSERT(tail.r_q16 > 40 * full.r_q16);
    ADCE_TEST_ASSERT(loop_implied_period_q16(&full, LOOP_TEETH_LONG) >
                     80 * loop_implied_period_q16(&tail, LOOP_WINDOW));

    /* D'. And the deflation is unbounded in amplitude, so it defeats R's
     * VERDICT and not merely its arithmetic. At amplitude 87 (0.13% of full
     * scale) the same construction puts R at 1.49 over the full window --
     * indistinguishable from the monotone decay's exact 1.0 by any threshold
     * that would still admit a settling trajectory -- while the trailing
     * window still reports the true R of 199. */
    loop_fill_transient_cycle(q, LOOP_TEETH_LONG, 100u, (adce_q16_t)52428,
                              (adce_q16_t)87);
    full = loop_metrics(q, LOOP_TEETH_LONG);
    tail = loop_metrics(q + (LOOP_TEETH_LONG - LOOP_WINDOW), LOOP_WINDOW);
    loop_report("D' hidden cycle full", &full, LOOP_TEETH_LONG);
    loop_report("D'  same, trailing W", &tail, LOOP_WINDOW);

    r_hidden_full = full.r_q16;
    ADCE_TEST_ASSERT(r_hidden_full < 3 * ADCE_Q16_ONE / 2); /* R < 1.5 */
    ADCE_TEST_ASSERT(r_hidden_full > ADCE_Q16_ONE);
    ADCE_TEST_ASSERT(tail.r_q16 == (adce_q16_t)(LOOP_WINDOW - 1u) *
                                       ADCE_Q16_ONE);
    ADCE_TEST_ASSERT(tail.r_q16 > 100 * r_hidden_full);

    /* --- E. DC HAS NO DERIVABLE DEADBAND ------------------------------- *
     * A 1-LSB dither is the smallest movement the publication path can
     * represent, and DC as specified in section 3 counts every one of its
     * diffs: DC grows linearly with the window, which is precisely the
     * signature the document assigns to a limit cycle.
     *
     * The assertion below is the finding, and it is exact: the full-scale
     * square wave of case A and this 1-LSB dither have the SAME DC and the
     * SAME R. Both metrics are amplitude-blind -- R by construction, since it
     * is a ratio, and DC because it counts signs. So neither can be given a
     * deadband from its own definition; the only quantity that separates a
     * 65536-LSB limit cycle from a 1-LSB dither is PP, and a threshold on PP
     * is exactly the settle band B that section 5 records as having NO
     * derivation.
     *
     * The deadband question therefore REDUCES to B rather than being a
     * separate open number, and it is not derivable here for the same reason.
     * DC is reported as UNUSABLE as a settle criterion, and no threshold is
     * invented for it. */
    loop_fill_lsb_dither(p, LOOP_TEETH_W);
    dither = loop_metrics(p, LOOP_TEETH_W);
    loop_report("E 1-LSB dither", &dither, LOOP_TEETH_W);

    ADCE_TEST_ASSERT(dither.pp == 1);
    ADCE_TEST_ASSERT(dither.tv == LOOP_TEETH_W - 1u);
    ADCE_TEST_ASSERT(dither.dc == LOOP_TEETH_W - 2u);

    /* The whole finding, in two identities. */
    ADCE_TEST_ASSERT(dither.dc == sq.dc);
    ADCE_TEST_ASSERT(dither.r_q16 == sq.r_q16);
    ADCE_TEST_ASSERT(sq.pp == dither.pp * ADCE_PRESSURE_MAX);

    printf("    LOOP finding: DC and R are identical for a full-scale square"
           " wave and a 1-LSB dither\n"
           "    (DC=%llu, R=%.4f for both; PP differs by %lldx). DC has no"
           " deadband derivable from\n"
           "    its own definition -- the separating quantity is PP, whose"
           " threshold is the settle\n"
           "    band B that docs/closed-loop-harness.md section 5 records as"
           " underived. DC is\n"
           "    therefore UNUSABLE as a settle criterion and no threshold is"
           " invented for it here.\n",
           (unsigned long long)dither.dc,
           (double)dither.r_q16 / (double)ADCE_Q16_ONE,
           (long long)ADCE_PRESSURE_MAX);

    return 0;
}

/* =====================================================================
 * The rig's step response, MEASURED and never asserted.
 *
 * Printed from case 1's configuration so a reader can see what the trailing
 * window of case 4D is actually clear of on a real step -- section 5 lists
 * every step-response number as reportable only.
 * ===================================================================== */

static int test_loop_step_response_report(void) {
    static adce_q16_t t[LOOP_EPOCHS];
    static loop_rig_t rig;
    loop_metrics_t post;
    loop_metrics_t tail;
    loop_metrics_t clear;
    size_t post_n = LOOP_EPOCHS - ADCE_OBS_WARMUP_EPOCHS;
    size_t settled_at = LOOP_EPOCHS;
    size_t k;

    ADCE_TEST_ASSERT(loop_run(&rig, 1u, loop_ingress_correct, t) == 0);

    /* Where the step transient ENDS: the first epoch after the step at which
     * pressure is back at MIN and never leaves again. Measured from the
     * trajectory, and reported -- deriving it instead is the open item
     * docs/closed-loop-harness.md section 7 attaches to its case 5, and that
     * is a ramp question this step does not answer. */
    for (k = LOOP_EPOCHS; k > LOOP_STEP_EPOCH; k--) {
        if (t[k - 1] != ADCE_PRESSURE_MIN) {
            break;
        }
        settled_at = k - 1;
    }

    post = loop_metrics(t + ADCE_OBS_WARMUP_EPOCHS, post_n);
    tail = loop_metrics(t + (LOOP_EPOCHS - LOOP_WINDOW), LOOP_WINDOW);
    clear = loop_metrics(t + settled_at, LOOP_EPOCHS - settled_at);

    printf("  LOOP step response (REPORTED, not asserted) step at epoch %u,"
           " rate x%u, transient ends at epoch %llu (%llu epochs)\n",
           LOOP_STEP_EPOCH, LOOP_STEP_MULT, (unsigned long long)settled_at,
           (unsigned long long)(settled_at - LOOP_STEP_EPOCH));
    loop_report("post-warmup", &post, post_n);
    loop_report("trailing W", &tail, LOOP_WINDOW);
    loop_report("clear of transient", &clear, LOOP_EPOCHS - settled_at);

    /* THE POINT OF THIS CASE, and the half of case 4D that a fabricated
     * sequence cannot supply: a window opened after the measured end of the
     * transient is EXACTLY clear of it on a real step -- PP is 0, so R is 0
     * by its own definition and DC is 0. That is what case 4D's trailing
     * window relies on, shown against the code rather than against a
     * hand-built array.
     *
     * Note that the LOOP_WINDOW trailing window above does NOT have this
     * property here: it opens at epoch 200, exactly where the step lands, so
     * it still contains the whole transient. It reports R = 1 and DC = 0
     * anyway -- correctly, because a lone transient is MONOTONE. R deflation
     * needs a transient mixed with a CYCLE, which is why 4D has to fabricate
     * one: this rig does not produce a limit cycle to mix in. */
    ADCE_TEST_ASSERT(clear.pp == 0);
    ADCE_TEST_ASSERT(clear.r_q16 == 0);
    ADCE_TEST_ASSERT(clear.dc == 0);

    /* Structural only, in the shape t_adce_latency.c uses: assert that the
     * fixture drove what it claims, never that a number landed in a band. */
    ADCE_TEST_ASSERT(rig.obs.publications ==
                     LOOP_EPOCHS - ADCE_OBS_WARMUP_EPOCHS + 1u);
    ADCE_TEST_ASSERT(post.pp > 0);
    ADCE_TEST_ASSERT(settled_at > LOOP_STEP_EPOCH);
    ADCE_TEST_ASSERT(settled_at < LOOP_EPOCHS);

    return 0;
}

/* =====================================================================
 * Confronting the section 2B derivation with the code. REPORT ONLY.
 *
 * Section 2B claims that on a geometric ramp the EWMA reaches a steady
 * solution in which z is CONSTANT and independent of the absolute rate, with
 *
 *     v = (1-a)a / (1 - (1-a)/(1+g)^2)      and      z = 1/sqrt(v)
 *
 * Every number that derivation produces rests on it -- g* = 8.98%, sup z =
 * 7.178, and through sup z the committed _Static_assert pinning N below 125.
 * None of it had ever been run against the implementation.
 *
 * NOTHING BELOW IS ASSERTED. The only checks are structural, in the shape
 * t_adce_latency.c uses: that the fixture ran, that the ramp did not overflow,
 * and that the writer was claimed. No measured value gates the build, because
 * a threshold on agreement would be a band and this project does not assert
 * bands.
 * ===================================================================== */

/* Measurement precision, stated as a requirement with its consequence rather
 * than chosen: at eps = 1e-5 the comparison resolves z to FIVE significant
 * figures, which is the precision the predicted values 1.7266 and 4.0560 are
 * quoted to. A looser eps could not distinguish the last quoted digit; a
 * tighter one would demand a longer prime than the ramp can represent. */
#define LOOP_RAMP_EPS 1e-5

#define LOOP_RAMP_MAX_EPOCHS 700u

/* The fixed point of the recurrence the code actually uses:
 * var = (1-a)*(var + a*d*d), with the (1-a) OUTSIDE the bracket. */
static double loop_ramp_v_code_form(double g) {
    const double a = ADCE_OBS_ALPHA;
    return (1.0 - a) * a / (1.0 - (1.0 - a) / ((1.0 + g) * (1.0 + g)));
}

/* The fixed point of the OTHER common form, var = (1-a)*var + a*d*d. Computed
 * so the measurement can discriminate rather than merely agree: the two forms
 * differ by exactly 1/sqrt(1-a) at EVERY g, so a measurement good to better
 * than 1% picks one of them outright. */
static double loop_ramp_v_other_form(double g) {
    const double a = ADCE_OBS_ALPHA;
    return a / (1.0 - (1.0 - a) / ((1.0 + g) * (1.0 + g)));
}

/* PRIME LENGTH, DERIVED. The cold-start transient from mu = var = 0 decays as
 * (1-a)^k, so reaching a relative error eps takes k = ln(eps)/ln(1-a) epochs.
 * At a = 2/101 that is ln(eps)/-0.02000067. Closed form, no trajectory
 * inspected -- which is the whole point, since choosing it by looking at one
 * would make it the fifth tuned number in this project. */
static uint32_t loop_ramp_prime_coldstart(double eps) {
    return (uint32_t)ceil(log(eps) / log(1.0 - ADCE_OBS_ALPHA));
}

/* The RAMP-SPECIFIC prime, also closed form, and needed because the bound
 * above is unreachable at large g -- see the overflow note in the report.
 *
 * z is a ratio, so what has to decay is the RELATIVE transient, and on a ramp
 * the signal grows underneath it: the mu error decays as (1-a)^k in absolute
 * terms but as ((1-a)/(1+g))^k relative to r_k, and the var error faster
 * still. The prefactor is NOT 1 and dropping it makes the bound too short --
 * at g = 0.02 the naive form gives 290 epochs where the recurrence needs 306.
 * The initial relative error is exact rather than observed: mu = var = 0 gives
 * d_0 = r_0 and var_0 = (1-a)a r_0^2, so z_0 = 1/sqrt((1-a)a) = sup z, and the
 * prefactor is |z_0/z_inf - 1|. All four terms are closed form. */
static uint32_t loop_ramp_prime_ramp(double eps, double g) {
    const double a = ADCE_OBS_ALPHA;
    double z0 = 1.0 / sqrt((1.0 - a) * a);
    double zinf = 1.0 / sqrt(loop_ramp_v_code_form(g));
    double prefactor = fabs(z0 / zinf - 1.0);

    return (uint32_t)ceil(log(eps / prefactor) / log((1.0 - a) / (1.0 + g)));
}

/* Exactly what n calls to adce_obs_tap would leave behind. The tap is a
 * relaxed fetch_add and adce_obs_counter_take is an exchange, so the only
 * thing epoch close ever reads is the total; storing it and accumulating it
 * are the same value by construction.
 *
 * Tapping for real is not an option here, and the number is the reason rather
 * than the excuse: a geometric ramp at g = 0.02 over 700 epochs offers
 * 5.24e14 arrivals, about 30 DAYS at the ~5 ns per-arrival cost measured in
 * docs/enforcement-plane.md section 5. A geometric ramp is structurally unable
 * to use the per-arrival path at any prime length worth having, which is a
 * fact about the ramp rather than a shortcut taken here -- and it is a real
 * constraint on how the document's cases 5 and 6 can ever be built. */
static void loop_ramp_counter_set(adce_obs_counter_t *counter, uint64_t n) {
    atomic_store_explicit(&counter->arrivals, n, memory_order_relaxed);
}

typedef struct {
    const char *label;
    double g;
    uint64_t n0;
    uint32_t epochs;
    uint32_t prime;
} loop_ramp_cfg_t;

/* One run's worth of output. Carries the PUBLISHED pressure as well as z,
 * because cases 5 and 6 are claims about what Enforcement would read, not
 * about the statistic: the squash and the publication clamp sit between them,
 * and warmup suppresses publication entirely for the first
 * ADCE_OBS_WARMUP_EPOCHS. Asserting on z would skip all three. */
typedef struct {
    double z_code[LOOP_RAMP_MAX_EPOCHS];
    double z_ref[LOOP_RAMP_MAX_EPOCHS];
    adce_q16_t pressure[LOOP_RAMP_MAX_EPOCHS];
    uint64_t n[LOOP_RAMP_MAX_EPOCHS];
    uint64_t publications;
    uint64_t last_epoch_id;
} loop_ramp_out_t;

/* The two ramps, defined once and shared by the report and by cases 5 and 6,
 * so the configuration a number was measured under cannot drift away from the
 * configuration it is asserted under.
 *
 * g = 0.02 is well below g* ~ 0.0898 (z = 1.7266, under z_lo = 3); g = 0.20 is
 * well above it (z = 4.0560, between z_lo and z_hi). Both are the document's
 * own choices and both are now measured, not predicted.
 *
 * The epoch counts are set by what the arrival type can hold, not by taste:
 * at g = 0.20 with n0 = 1e6 the count passes UINT64_MAX at epoch 168, so 160
 * is the last round number that fits with headroom. */
static const loop_ramp_cfg_t loop_ramp_below = {"g=0.02", 0.02, 10000000u,
                                                700u, 576u};
static const loop_ramp_cfg_t loop_ramp_above = {"g=0.20", 0.20, 1000000u,
                                                160u, 100u};

/* Drives the real adce_obs_epoch_close down a geometric ramp and, alongside
 * it, a transcription of that function's arithmetic fed the SAME integer
 * arrival counts.
 *
 * The transcription is NOT an independent implementation and is not offered as
 * one -- it is the same expressions in the same order. What it establishes is
 * narrower and still worth having: that the arithmetic is actually REACHED,
 * with warmup not suppressing it, the sigma floor not clamping, and the
 * post-update variance being what sqrt() sees. The independent check is the
 * comparison against the section 2B closed form, which shares no code with
 * either. */
static int loop_ramp_run(const loop_ramp_cfg_t *cfg, loop_ramp_out_t *out) {
    static adce_obs_counter_t counter;
    static adce_epoch_state_t epoch;
    static adce_obs_ctx_t obs;
    double mu = 0.0;
    double var = 0.0;
    uint32_t k;

    if (cfg->epochs > LOOP_RAMP_MAX_EPOCHS) {
        return 4;
    }

    memset(out, 0, sizeof(*out));
    memset(&counter, 0, sizeof(counter));
    memset(&epoch, 0, sizeof(epoch));
    adce_obs_init(&obs, &counter, &epoch);
    if (!adce_obs_claim_writer(&obs)) {
        return 1;
    }

    for (k = 0; k < cfg->epochs; k++) {
        double x = (double)cfg->n0 * pow(1.0 + cfg->g, (double)k);
        uint64_t n;
        double rate;
        double d;
        double sigma;

        /* Structural: a ramp that overruns the arrival type would measure the
         * wraparound rather than the recurrence. Headroom below UINT64_MAX so
         * the +0.5 rounding cannot cross it. */
        if (!(x < 9.0e18)) {
            return 2;
        }
        n = (uint64_t)(x + 0.5);

        loop_ramp_counter_set(&counter, n);
        if (adce_obs_epoch_close(&obs, (uint64_t)(k + 1u) *
                                           ADCE_OBS_EPOCH_NS) < 0) {
            return 3;
        }
        out->z_code[k] = obs.last_z;
        out->n[k] = n;

        /* The published value, read back through the same seqlock Enforcement
         * would use. Single-threaded, so it cannot tear; before the first
         * publication it returns the zeroed state, which IS the published
         * value there -- warmup publishes nothing and that is the design. */
        {
            adce_q16_t p = 0;
            uint64_t id = 0;
            uint64_t at = 0;

            if (!adce_epoch_read(&epoch, &p, &id, &at)) {
                return 5;
            }
            out->pressure[k] = p;
            out->last_epoch_id = id;
        }

        rate = (double)n / ADCE_OBS_EPOCH_SECONDS;
        d = rate - mu;
        mu = mu + ADCE_OBS_ALPHA * d;
        var = (1.0 - ADCE_OBS_ALPHA) * (var + ADCE_OBS_ALPHA * d * d);
        sigma = sqrt(var);
        if (!(sigma >= ADCE_OBS_SIGMA_EPSILON)) {
            sigma = ADCE_OBS_SIGMA_EPSILON;
        }
        out->z_ref[k] = d / sigma;
    }

    out->publications = obs.publications;
    return 0;
}

static int test_loop_ramp_fixed_point_report(void) {
    static loop_ramp_out_t out;
    /* g = 0.02 primes to the cold-start bound, which it can afford. g = 0.20
     * cannot: see the report line below. */
    const loop_ramp_cfg_t *cfgs[2] = {&loop_ramp_below, &loop_ramp_above};
    size_t c;

    printf("  LOOP ramp fixed point -- REPORT ONLY, nothing below is asserted\n");
    printf("    eps=%.0e -> z resolved to 5 significant figures, the precision"
           " the predictions are quoted to\n",
           LOOP_RAMP_EPS);
    printf("    a=%.10f  ln(1-a)=%.8f  cold-start prime k=ln(eps)/ln(1-a)="
           "%u epochs\n",
           ADCE_OBS_ALPHA, log(1.0 - ADCE_OBS_ALPHA),
           loop_ramp_prime_coldstart(LOOP_RAMP_EPS));
    printf("    the two candidate recurrences differ by exactly 1/sqrt(1-a)="
           "%.6f at EVERY g\n",
           1.0 / sqrt(1.0 - ADCE_OBS_ALPHA));

    for (c = 0; c < sizeof(cfgs) / sizeof(cfgs[0]); c++) {
        const loop_ramp_cfg_t *cfg = cfgs[c];
        double pred_2b = 1.0 / sqrt(loop_ramp_v_code_form(cfg->g));
        double pred_other = 1.0 / sqrt(loop_ramp_v_other_form(cfg->g));
        double worst_impl = 0.0;
        double worst_steady = 0.0;
        uint32_t worst_at = 0;
        uint64_t n_last = 0;
        uint32_t k;
        int rc;

        rc = loop_ramp_run(cfg, &out);
        ADCE_TEST_ASSERT(rc == 0);
        n_last = out.n[cfg->epochs - 1u];

        for (k = 0; k < cfg->epochs; k++) {
            double impl = fabs(out.z_code[k] - out.z_ref[k]) /
                          (fabs(out.z_ref[k]) > 0.0 ? fabs(out.z_ref[k]) : 1.0);
            if (impl > worst_impl) {
                worst_impl = impl;
            }
            if (k >= cfg->prime) {
                double dev = fabs(out.z_code[k] / pred_2b - 1.0);
                if (dev > worst_steady) {
                    worst_steady = dev;
                    worst_at = k;
                }
            }
        }

        printf("\n    %s  n0=%llu epochs=%u prime=%u (derived: cold-start %u,"
               " ramp-specific %u)  n_last=%.3e\n",
               cfg->label, (unsigned long long)cfg->n0, cfg->epochs,
               cfg->prime, loop_ramp_prime_coldstart(LOOP_RAMP_EPS),
               loop_ramp_prime_ramp(LOOP_RAMP_EPS, cfg->g), (double)n_last);
        printf("      predicted 2B (code form)   z = %.8f\n", pred_2b);
        printf("      predicted other recurrence z = %.8f\n", pred_other);
        printf("      MEASURED steady z          z = %.8f  (epoch %u)\n",
               out.z_code[cfg->epochs - 1u], cfg->epochs - 1u);
        printf("      rel err vs 2B    = %.3e   %s\n",
               fabs(out.z_code[cfg->epochs - 1u] / pred_2b - 1.0),
               fabs(out.z_code[cfg->epochs - 1u] / pred_2b - 1.0) < LOOP_RAMP_EPS
                   ? "WITHIN eps"
                   : "OUTSIDE eps");
        printf("      rel err vs other = %.3e\n",
               fabs(out.z_code[cfg->epochs - 1u] / pred_other - 1.0));
        printf("      code vs transcribed recurrence, max over ALL %u epochs"
               " = %.3e\n",
               cfg->epochs, worst_impl);
        printf("      WORST deviation from the constant across the whole"
               " steady window [%u,%u) = %.3e at epoch %u\n",
               cfg->prime, cfg->epochs, worst_steady, worst_at);

        printf("      epoch-by-epoch:\n");
        for (k = 0; k < cfg->epochs; k = (k == 0u) ? 1u : k * 2u) {
            printf("        k=%4u  z=%.8f  rel vs 2B=%+.3e\n", k, out.z_code[k],
                   out.z_code[k] / pred_2b - 1.0);
        }
        printf("        k=%4u  z=%.8f  rel vs 2B=%+.3e\n", cfg->epochs - 1u,
               out.z_code[cfg->epochs - 1u],
               out.z_code[cfg->epochs - 1u] / pred_2b - 1.0);
    }

    printf("\n    z at epoch 0 from mu=var=0 is %.8f = 1/sqrt((1-a)a) = sup z,"
           " which is section 7's\n"
           "    cold-start transient as an exact number rather than the"
           " approximate 7.18 it quotes.\n",
           1.0 / sqrt((1.0 - ADCE_OBS_ALPHA) * ADCE_OBS_ALPHA));

    return 0;
}

/* =====================================================================
 * Cases 5 and 6 -- the ramp, as assertions.
 *
 * SCOPE, and why it is narrower than docs/closed-loop-harness.md section 7
 * originally specified. Case 5 there asserts two things: that pressure stays
 * at ADCE_PRESSURE_MIN across the ramp, AND that admitted stays under the
 * bucket ceiling. Only the first is written here.
 *
 * The reason is structural and was measured rather than guessed. A geometric
 * ramp cannot drive the per-arrival path at all -- the g = 0.02 ramp offers
 * 5.24e14 arrivals, about 30 days at the ~5 ns gate cost -- so the counter is
 * injected and THE GATE IS NOT IN THE LOOP. Writing the ceiling half anyway
 * would mean inventing a fixture whose only purpose is to make the claim
 * assertable, which is fitting the evidence to the assertion.
 *
 * The narrowing costs nothing, because the two halves belong to two different
 * mechanisms and CLAUDE.md already records the split: the z-score detector is
 * a FAST-TRANSIENT detector only, and the token bucket is the sole defence
 * against sustained or slowly-growing load. What a ramp demonstrates is
 * OBSERVATION GOING BLIND -- which is exactly case 5's pressure half. The
 * ceiling claim belongs to the bucket, and it is already evidenced elsewhere:
 * test_harness_concurrent asserts admitted <= rate*elapsed + capacity per
 * thread under live four-thread load, with the measured admitted sitting
 * within a handful of arrivals of the ceiling. Not unevidenced -- evidenced by
 * the fixture that can actually run the gate.
 * ===================================================================== */

/* Case 5's predicate. Returns 0 when pressure is pinned at ADCE_PRESSURE_MIN
 * across [lo, hi), 1 otherwise, reporting on stderr the way every other
 * predicate in this project reports so the teeth fence can cover it. */
static int loop_ramp_check_floor(const loop_ramp_out_t *out, uint32_t lo,
                                 uint32_t hi, const char *label) {
    uint32_t k;

    for (k = lo; k < hi; k++) {
        if (out->pressure[k] != ADCE_PRESSURE_MIN) {
            fprintf(stderr,
                    "FAIL: %s pressure left the floor at epoch %u: %lld"
                    " (expected %lld)\n",
                    label, k, (long long)out->pressure[k],
                    (long long)ADCE_PRESSURE_MIN);
            return 1;
        }
    }
    return 0;
}

/* Case 6's predicate: the mirror. */
static int loop_ramp_check_lifted(const loop_ramp_out_t *out, uint32_t lo,
                                  uint32_t hi, const char *label) {
    uint32_t k;

    for (k = lo; k < hi; k++) {
        if (out->pressure[k] <= ADCE_PRESSURE_MIN) {
            fprintf(stderr,
                    "FAIL: %s pressure did not leave the floor at epoch %u:"
                    " %lld\n",
                    label, k, (long long)out->pressure[k]);
            return 1;
        }
    }
    return 0;
}

/* The OTHER half of case 5, and the half that carries its whole content.
 *
 * "Pressure stays at ADCE_PRESSURE_MIN" is satisfied trivially by a system
 * under no load at all: a FLAT arrival sequence drives d to zero, sigma to the
 * epsilon floor and z to zero, so pressure sits at MIN with nothing growing
 * anywhere. The claim worth asserting is that pressure stays at MIN WHILE
 * OFFERED VOLUME GROWS, so the growth is asserted rather than described --
 * otherwise case 5 would pass on a rig that had stopped. */
static int loop_ramp_check_growth(const loop_ramp_out_t *out, uint32_t lo,
                                  uint32_t hi, uint64_t factor,
                                  const char *label) {
    uint64_t first;
    uint64_t last;

    if (hi <= lo) {
        fprintf(stderr, "FAIL: %s empty window [%u,%u)\n", label, lo, hi);
        return 1;
    }
    first = out->n[lo];
    last = out->n[hi - 1u];

    if (first == 0u || last / first < factor) {
        fprintf(stderr,
                "FAIL: %s offered volume grew %llux over [%u,%u), needed"
                " %llux (%llu -> %llu)\n",
                label, (unsigned long long)(first ? last / first : 0u), lo, hi,
                (unsigned long long)factor, (unsigned long long)first,
                (unsigned long long)last);
        return 1;
    }
    return 0;
}

/* The window both cases assert over. It opens at the DERIVED prime, and at
 * warmup's end when that is later -- there is no published pressure before the
 * first publication, so a window opening earlier would be reading the zeroed
 * epoch state and calling it evidence.
 *
 * The prime is the PREFACTOR-CORRECTED ramp-specific length,
 * ln(eps/|z_0/z_inf - 1|)/ln((1-a)/(1+g)) -- 319 epochs at g = 0.02, 56 at
 * g = 0.20. Not the naive ln(eps)/ln((1-a)/(1+g)), which drops the prefactor
 * and gives 290 at g = 0.02 where the recurrence measurably needs 306: too
 * short, and short in the direction that would let a still-decaying transient
 * be asserted on as if it were the steady state. And not the cold-start bound
 * of 576 either, which is correct but unreachable at g = 0.20 -- priming that
 * long there needs 1.2^576 = 4.06e45 arrivals, 26 orders past UINT64_MAX. One
 * bound has to serve both cases, and this is the one that does. */
static uint32_t loop_ramp_window_lo(double g) {
    uint32_t prime = loop_ramp_prime_ramp(LOOP_RAMP_EPS, g);

    return prime > (uint32_t)ADCE_OBS_WARMUP_EPOCHS
               ? prime
               : (uint32_t)ADCE_OBS_WARMUP_EPOCHS;
}

/* Case 5. A ramp well below g* holds pressure at the floor while offered
 * volume grows without bound -- section 1.2's claim as an executed fact. */
static int test_loop_ramp_below_threshold(void) {
    static loop_ramp_out_t out;
    const loop_ramp_cfg_t *cfg = &loop_ramp_below;
    uint32_t lo = loop_ramp_window_lo(cfg->g);
    uint32_t hi = cfg->epochs;

    ADCE_TEST_ASSERT(loop_ramp_run(cfg, &out) == 0);

    /* Liveness, so "pressure is at MIN" cannot be satisfied by an observer
     * that never published or an epoch state that was never written. Without
     * this the floor predicate would pass on a frozen rig. */
    ADCE_TEST_ASSERT(out.publications == cfg->epochs - ADCE_OBS_WARMUP_EPOCHS + 1u);
    ADCE_TEST_ASSERT(out.last_epoch_id == cfg->epochs);

    ADCE_TEST_ASSERT(loop_ramp_check_floor(&out, lo, hi, "ramp-below") == 0);
    ADCE_TEST_ASSERT(loop_ramp_check_growth(&out, lo, hi, 100u,
                                            "ramp-below") == 0);

    printf("  LOOP ramp below g* (%s) window [%u,%u) prime=%u: pressure pinned"
           " at %lld while offered volume grew %llux (%llu -> %llu per epoch),"
           " z=%.6f\n",
           cfg->label, lo, hi, loop_ramp_prime_ramp(LOOP_RAMP_EPS, cfg->g),
           (long long)ADCE_PRESSURE_MIN,
           (unsigned long long)(out.n[hi - 1u] / out.n[lo]),
           (unsigned long long)out.n[lo], (unsigned long long)out.n[hi - 1u],
           out.z_code[hi - 1u]);

    return 0;
}

/* Case 6. The same rig above g*, which is what stops case 5 being a statement
 * about a detector that never fires at all. */
static int test_loop_ramp_above_threshold(void) {
    static loop_ramp_out_t out;
    const loop_ramp_cfg_t *cfg = &loop_ramp_above;
    uint32_t lo = loop_ramp_window_lo(cfg->g);
    uint32_t hi = cfg->epochs;

    ADCE_TEST_ASSERT(loop_ramp_run(cfg, &out) == 0);

    ADCE_TEST_ASSERT(out.publications == cfg->epochs - ADCE_OBS_WARMUP_EPOCHS + 1u);
    ADCE_TEST_ASSERT(out.last_epoch_id == cfg->epochs);

    ADCE_TEST_ASSERT(loop_ramp_check_lifted(&out, lo, hi, "ramp-above") == 0);

    /* Below the clamp as well as above the floor. Section 2B's sup z is 7.178
     * against z_hi = 8, so NO ramp at any growth rate can saturate the squash;
     * a pressure of exactly ADCE_PRESSURE_MAX here would mean that property
     * had failed, and it is the property the committed N < 125 _Static_assert
     * exists to protect. Asserting the bound rather than the value, because
     * the value is a measurement. */
    ADCE_TEST_ASSERT(out.pressure[hi - 1u] < ADCE_PRESSURE_MAX);

    printf("  LOOP ramp above g* (%s) window [%u,%u) prime=%u: pressure lifted"
           " to %lld of %lld (z=%.6f, sup z=%.4f < z_hi=%d so the squash"
           " cannot saturate)\n",
           cfg->label, lo, hi, loop_ramp_prime_ramp(LOOP_RAMP_EPS, cfg->g),
           (long long)out.pressure[hi - 1u], (long long)ADCE_PRESSURE_MAX,
           out.z_code[hi - 1u],
           1.0 / sqrt((1.0 - ADCE_OBS_ALPHA) * ADCE_OBS_ALPHA),
           ADCE_OBS_Z_HI_INT);

    return 0;
}

/* The teeth for both, in the shape test_harness_stale_split_teeth established:
 * feed each predicate the data that must break it, and observe the rejection.
 *
 * TWO mutations, not one, because the obvious mutation does not close case 5's
 * actual hole. A ramp above g* breaks the PRESSURE half and is the natural
 * counterfactual -- but a FLAT load also holds pressure at the floor, with no
 * growth at all, so the pressure half alone is satisfied by a system doing
 * nothing. The growth half is what distinguishes a ramp from a stopped rig,
 * and it needs its own mutation to show it has teeth. */
static int test_loop_ramp_teeth(void) {
    static loop_ramp_out_t below;
    static loop_ramp_out_t above;
    static loop_ramp_out_t flat;
    /* Same n0 and epoch count as the below-threshold ramp, g = 0 exactly. */
    static const loop_ramp_cfg_t cfg_flat = {"g=0.00", 0.0, 10000000u, 700u,
                                             576u};
    uint32_t lo_b = loop_ramp_window_lo(loop_ramp_below.g);
    uint32_t lo_a = loop_ramp_window_lo(loop_ramp_above.g);

    fprintf(stderr,
            "  LOOP ramp teeth BEGIN -- every FAIL line until 'teeth END' is"
            " EXPECTED;\n"
            "  loop_ramp_check_floor / _lifted / _growth are the code under"
            " test here.\n");

    ADCE_TEST_ASSERT(loop_ramp_run(&loop_ramp_below, &below) == 0);
    ADCE_TEST_ASSERT(loop_ramp_run(&loop_ramp_above, &above) == 0);
    ADCE_TEST_ASSERT(loop_ramp_run(&cfg_flat, &flat) == 0);

    /* 1. Case 5's floor predicate, fed the above-threshold ramp. Must reject:
     *    if it did not, case 5 would be passing on a detector that cannot
     *    tell 8.98%/epoch growth from 20%/epoch growth. */
    ADCE_TEST_ASSERT(
        loop_ramp_check_floor(&above, lo_a, loop_ramp_above.epochs,
                              "teeth-floor-vs-above") == 1);

    /* 2. Case 6's lifted predicate, fed the below-threshold ramp. The mirror,
     *    and it is what stops case 6 passing on a detector stuck high. */
    ADCE_TEST_ASSERT(
        loop_ramp_check_lifted(&below, lo_b, loop_ramp_below.epochs,
                               "teeth-lifted-vs-below") == 1);

    /* 3. THE HOLE THE OBVIOUS MUTATION MISSES. A flat load passes case 5's
     *    floor predicate outright -- pressure never leaves MIN, because there
     *    is nothing to detect. Only the growth half rejects it. This is the
     *    region where the weaker predicate cannot see the defect at all, the
     *    same shape as 0 < aged <= bound was for the summed stale bound. */
    ADCE_TEST_ASSERT(loop_ramp_check_floor(&flat, lo_b, cfg_flat.epochs,
                                           "teeth-flat-passes-floor") == 0);
    ADCE_TEST_ASSERT(loop_ramp_check_growth(&flat, lo_b, cfg_flat.epochs, 100u,
                                            "teeth-flat-vs-growth") == 1);

    /* 4. And the growth predicate must not reject the real ramp, or it would
     *    be rejecting everything rather than discriminating. */
    ADCE_TEST_ASSERT(loop_ramp_check_growth(&below, lo_b,
                                            loop_ramp_below.epochs, 100u,
                                            "teeth-growth-vs-below") == 0);

    fprintf(stderr,
            "  LOOP ramp teeth END -- expected failures above. Flat load"
            " pressure at epoch %u is %lld, which PASSES the floor predicate"
            " and is\n"
            "  why case 5 asserts growth as well.\n",
            cfg_flat.epochs - 1u,
            (long long)flat.pressure[cfg_flat.epochs - 1u]);

    return 0;
}

/* =====================================================================
 * External forwarders; see the header comment.
 * ===================================================================== */

#define ADCE_LOOP_TEST_EXPORT(name)                                          \
    int adce_t_##name(void);                                                 \
    int adce_t_##name(void) { return test_##name(); }

ADCE_LOOP_TEST_EXPORT(loop_synthetic_determinism)
ADCE_LOOP_TEST_EXPORT(loop_draw_invariance)
ADCE_LOOP_TEST_EXPORT(loop_inverted_draw_dependence)
ADCE_LOOP_TEST_EXPORT(loop_settle_metrics_teeth)
ADCE_LOOP_TEST_EXPORT(loop_step_response_report)
ADCE_LOOP_TEST_EXPORT(loop_ramp_fixed_point_report)
ADCE_LOOP_TEST_EXPORT(loop_ramp_below_threshold)
ADCE_LOOP_TEST_EXPORT(loop_ramp_above_threshold)
ADCE_LOOP_TEST_EXPORT(loop_ramp_teeth)
