/* Enforcement Plane unit tests.
 *
 * The runner and main() live in t_adce_platform.c. Each case here is static,
 * which is what scripts/verify.sh's guard matches on when it derives the
 * expected test set from the source; static also means internal linkage, so
 * each case gets one external forwarder at the bottom of this file. */

#include "../include/adce_enforce.h"

#include <math.h>
#include <stdio.h>

#define ADCE_TEST_ASSERT(cond)                                               \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
            return 1;                                                        \
        }                                                                    \
    } while (0)

static adce_epoch_state_t g_enf_epoch;

/* A draw whose top 16 bits are exactly `top`. Every decision in this file is
 * driven through one of these, so each assertion names the pressure it is
 * discriminating between rather than relying on a random stream. */
static uint64_t enf_draw(uint64_t top) {
    return top << 48;
}

/* The same, with every low bit set. The shed decision must be identical: only
 * the top 16 bits may participate. */
static uint64_t enf_draw_dirty(uint64_t top) {
    return (top << 48) | (((uint64_t)1 << 48) - 1);
}

static void enf_fresh(adce_enf_ctx_t *ctx, uint64_t now_ns) {
    memset(&g_enf_epoch, 0, sizeof(g_enf_epoch));
    adce_enf_thread_init(ctx, &g_enf_epoch, now_ns);
}

/* =====================================================================
 * 1. The shed mapping, exhaustively.
 * ===================================================================== */

static int test_enf_shed_mapping(void) {
    static const adce_q16_t pressures[] = {0, 1, 16384, 32768, 65535, 65536};
    size_t i;

    for (i = 0; i < sizeof(pressures) / sizeof(pressures[0]); ++i) {
        adce_q16_t p = pressures[i];
        long shed = 0;
        long top;

        /* Every one of the 65536 buckets, so this is an exact count rather
         * than an estimate. Catches a wrong shift width, an off-by-one at
         * either endpoint, and modulo bias -- none of which a statistical
         * test at this resolution could separate. */
        for (top = 0; top < 65536; ++top) {
            if (adce_enf_should_shed(p, enf_draw((uint64_t)top))) {
                shed++;
            }
            /* The low 48 bits must not participate. If the mapping used
             * draw & 0xFFFF instead, the FRACTION would still come out
             * right from a uniform stream and only the independence between
             * successive decisions would be wrong -- so the fraction tests
             * below could never catch it. This can. */
            ADCE_TEST_ASSERT(
                adce_enf_should_shed(p, enf_draw((uint64_t)top)) ==
                adce_enf_should_shed(p, enf_draw_dirty((uint64_t)top)));
        }

        ADCE_TEST_ASSERT(shed == (long)p);
    }

    /* The two endpoints stated as the properties they are, not as counts. */
    ADCE_TEST_ASSERT(adce_enf_should_shed(ADCE_PRESSURE_MIN, UINT64_MAX) == 0);
    ADCE_TEST_ASSERT(adce_enf_should_shed(ADCE_PRESSURE_MAX, 0) != 0);
    ADCE_TEST_ASSERT(adce_enf_should_shed(ADCE_PRESSURE_MAX, UINT64_MAX) != 0);

    /* Total over the whole adce_q16_t domain: out-of-contract values clamp to
     * maximal rather than reaching the comparison unclamped. */
    ADCE_TEST_ASSERT(adce_enf_should_shed(-1, UINT64_MAX) != 0);
    ADCE_TEST_ASSERT(adce_enf_should_shed(ADCE_Q16_MIN, UINT64_MAX) != 0);
    ADCE_TEST_ASSERT(adce_enf_should_shed(ADCE_Q16_MAX, UINT64_MAX) != 0);

    return 0;
}

/* =====================================================================
 * 2. Monotonicity.
 * ===================================================================== */

static int test_enf_shed_monotone(void) {
    long top;

    /* For a fixed draw, once a pressure sheds it, no higher pressure may
     * admit it: the shedding set only grows. A mapping that was monotone in
     * aggregate but not pointwise would let a pressure INCREASE admit
     * traffic it previously dropped. */
    for (top = 0; top < 65536; top += 7) {
        uint64_t draw = enf_draw((uint64_t)top);
        int prev = 0;
        adce_q16_t p;

        for (p = 0; p <= ADCE_PRESSURE_MAX; p += 251) {
            int now = adce_enf_should_shed(p, draw) != 0;
            ADCE_TEST_ASSERT(now >= prev);
            prev = now;
        }
        ADCE_TEST_ASSERT(adce_enf_should_shed(ADCE_PRESSURE_MAX, draw) != 0);
        ADCE_TEST_ASSERT(adce_enf_should_shed(ADCE_PRESSURE_MIN, draw) == 0);
    }

    return 0;
}

/* =====================================================================
 * 3. Re-clamp at read.
 * ===================================================================== */

static int test_enf_read_clamp(void) {
    adce_enf_ctx_t ctx;
    const uint64_t t0 = ADCE_ADVICE_TIMEOUT_NS * 4;

    /* Top-16 of 65535 sheds only at ADCE_PRESSURE_MAX. So a DROP_SHED here
     * proves the published value was read as MAXIMAL -- not as zero, and not
     * as the stale fallback of 32768. */
    enf_fresh(&ctx, t0);
    adce_epoch_publish(&g_enf_epoch, -1, 1, t0);
    ADCE_TEST_ASSERT(adce_enf_decide(&ctx, t0, enf_draw(65535)) ==
                     ADCE_ENF_DROP_SHED);

    enf_fresh(&ctx, t0);
    adce_epoch_publish(&g_enf_epoch, -ADCE_Q16_ONE, 1, t0);
    ADCE_TEST_ASSERT(adce_enf_decide(&ctx, t0, enf_draw(65535)) ==
                     ADCE_ENF_DROP_SHED);

    enf_fresh(&ctx, t0);
    adce_epoch_publish(&g_enf_epoch, ADCE_Q16_MIN, 1, t0);
    ADCE_TEST_ASSERT(adce_enf_decide(&ctx, t0, enf_draw(65535)) ==
                     ADCE_ENF_DROP_SHED);

    /* Out-of-range positive, including the ADCE_Q16_MAX a collapsed divisor
     * produces, clamps to maximal rather than wrapping. */
    enf_fresh(&ctx, t0);
    adce_epoch_publish(&g_enf_epoch, ADCE_Q16_MAX, 1, t0);
    ADCE_TEST_ASSERT(adce_enf_decide(&ctx, t0, enf_draw(65535)) ==
                     ADCE_ENF_DROP_SHED);

    enf_fresh(&ctx, t0);
    adce_epoch_publish(&g_enf_epoch, adce_q16_div(ADCE_Q16_ONE, 0), 1, t0);
    ADCE_TEST_ASSERT(adce_enf_decide(&ctx, t0, enf_draw(65535)) ==
                     ADCE_ENF_DROP_SHED);

    /* In-range passes through unchanged: 16384 sheds a draw just below it and
     * admits one at it. */
    enf_fresh(&ctx, t0);
    adce_epoch_publish(&g_enf_epoch, 16384, 1, t0);
    ADCE_TEST_ASSERT(adce_enf_decide(&ctx, t0, enf_draw(16383)) ==
                     ADCE_ENF_DROP_SHED);
    ADCE_TEST_ASSERT(adce_enf_decide(&ctx, t0, enf_draw(16384)) ==
                     ADCE_ENF_ADMIT);

    /* Zero admits everything, which is the other end of the contract. */
    enf_fresh(&ctx, t0);
    adce_epoch_publish(&g_enf_epoch, 0, 1, t0);
    ADCE_TEST_ASSERT(adce_enf_decide(&ctx, t0, 0) == ADCE_ENF_ADMIT);
    ADCE_TEST_ASSERT(adce_enf_decide(&ctx, t0, UINT64_MAX) == ADCE_ENF_ADMIT);
    ADCE_TEST_ASSERT(ctx.dropped_shed == 0);

    return 0;
}

/* =====================================================================
 * 4. The stale fallback, pinned to its exact value.
 * ===================================================================== */

static int test_enf_stale_fallback(void) {
    adce_enf_ctx_t ctx;
    const uint64_t t0 = ADCE_ADVICE_TIMEOUT_NS * 4;

    /* Published pressure is 0, so any shedding observed below came from the
     * substituted fallback and not from the epoch.
     *
     * Two draws bracket ADCE_ENF_STALE_PRESSURE == 32768 and together pin it
     * to that exact value: top-16 of 32767 sheds at 32768 but not at 0, and
     * top-16 of 32768 does NOT shed at 32768 but would at maximal. A fallback
     * of 0 fails the first; a fallback of ADCE_PRESSURE_MAX fails the second. */
    enf_fresh(&ctx, t0);
    adce_epoch_publish(&g_enf_epoch, 0, 1, t0);

    /* Exactly at the timeout is NOT stale: the watchdog compares strictly
     * greater. */
    ADCE_TEST_ASSERT(adce_enf_decide(&ctx, t0 + ADCE_ADVICE_TIMEOUT_NS,
                                     enf_draw(32767)) == ADCE_ENF_ADMIT);
    ADCE_TEST_ASSERT(ctx.stale_reads == 0);

    /* One nanosecond past it is. */
    ADCE_TEST_ASSERT(adce_enf_decide(&ctx, t0 + ADCE_ADVICE_TIMEOUT_NS + 1,
                                     enf_draw(32767)) == ADCE_ENF_DROP_SHED);
    ADCE_TEST_ASSERT(ctx.stale_reads == 1);

    /* Not maximal: the draw at 32768 must survive the fallback. */
    ADCE_TEST_ASSERT(adce_enf_decide(&ctx, t0 + ADCE_ADVICE_TIMEOUT_NS + 1,
                                     enf_draw(32768)) == ADCE_ENF_ADMIT);
    ADCE_TEST_ASSERT(ctx.stale_reads == 2);

    /* And not total drop: a stale gate still admits, which is the whole
     * argument against a maximal fallback. */
    ADCE_TEST_ASSERT(ctx.admitted > 0);

    /* A fresh publication ends the stale window without any separate reset. */
    adce_epoch_publish(&g_enf_epoch, 0, 2, t0 + ADCE_ADVICE_TIMEOUT_NS + 1);
    ADCE_TEST_ASSERT(adce_enf_decide(&ctx, t0 + ADCE_ADVICE_TIMEOUT_NS + 2,
                                     enf_draw(32767)) == ADCE_ENF_ADMIT);
    ADCE_TEST_ASSERT(ctx.stale_reads == 2);

    return 0;
}

/* =====================================================================
 * 5. Cold start, with no cold-start branch anywhere.
 * ===================================================================== */

static int test_enf_cold_start(void) {
    adce_enf_ctx_t ctx;
    const uint64_t t0 = ADCE_ADVICE_TIMEOUT_NS * 4;

    /* A zero-initialised epoch state has observed_at_ns == 0, so the ordinary
     * staleness comparison already reads it as stale. There is no cold-start
     * flag and no first-publication special case: the watchdog is the entire
     * mechanism, exactly as docs/observation-plane.md section 1 requires. */
    enf_fresh(&ctx, t0);

    ADCE_TEST_ASSERT(adce_enf_decide(&ctx, t0, enf_draw(32767)) ==
                     ADCE_ENF_DROP_SHED);
    ADCE_TEST_ASSERT(ctx.stale_reads == 1);
    ADCE_TEST_ASSERT(adce_enf_decide(&ctx, t0, enf_draw(32768)) ==
                     ADCE_ENF_ADMIT);

    /* The pressure sitting in the zeroed struct is 0, which alone would admit
     * everything. This asserts that the value is NOT what made the decision --
     * had the gate trusted it, the 32767 draw above would have been admitted. */
    {
        adce_q16_t pressure = 1;
        uint64_t epoch_id = 1;
        uint64_t observed_at_ns = 1;

        ADCE_TEST_ASSERT(adce_epoch_read(&g_enf_epoch, &pressure, &epoch_id,
                                         &observed_at_ns) == 1);
        ADCE_TEST_ASSERT(pressure == 0);
        ADCE_TEST_ASSERT(observed_at_ns == 0);
    }

    /* Known inherited property, recorded rather than worked around: staleness
     * is (now - observed_at) > TIMEOUT, so a zeroed epoch read at a now_ns
     * below the timeout is NOT stale. adce_now_ns() is CLOCK_MONOTONIC_RAW and
     * boot-relative, so a real deployment is past that within 50 ms of boot.
     * The behaviour belongs to adce_epoch_is_stale in adce_platform.h and this
     * plane cannot and should not paper over it. */
    ADCE_TEST_ASSERT(adce_epoch_is_stale(0, ADCE_ADVICE_TIMEOUT_NS) == 0);
    ADCE_TEST_ASSERT(adce_epoch_is_stale(0, ADCE_ADVICE_TIMEOUT_NS + 1) == 1);

    return 0;
}

/* =====================================================================
 * 6. The absolute ceiling holds while the detector says nothing is wrong.
 * ===================================================================== */

#define ADCE_ENF_T_ARRIVALS 40000
#define ADCE_ENF_T_STEP_NS 1000

static int test_enf_bucket_ceiling(void) {
    adce_enf_ctx_t ctx;
    const uint64_t t0 = ADCE_ADVICE_TIMEOUT_NS * 4;
    uint64_t elapsed_ns;
    uint64_t bound;
    int i;

    enf_fresh(&ctx, t0);
    adce_epoch_publish(&g_enf_epoch, 0, 1, t0);

    /* Pressure is 0 throughout: the detector reports calm. This is the slow
     * ramp of docs/enforcement-plane.md section 1.2 -- the attacker raised the
     * mean along with the rate, so the z-score never alarmed. Nothing here may
     * depend on the detector agreeing that anything is wrong. */
    for (i = 1; i <= ADCE_ENF_T_ARRIVALS; ++i) {
        uint64_t now = t0 + (uint64_t)i * ADCE_ENF_T_STEP_NS;

        /* UINT64_MAX is the draw least likely to shed at any pressure, so any
         * drop counted below came from the bucket, never from stage one. */
        (void)adce_enf_decide(&ctx, now, UINT64_MAX);

        /* The window stays inside the advice timeout, so the epoch never goes
         * stale and the fallback pressure never enters the measurement. */
        ADCE_TEST_ASSERT(ctx.stale_reads == 0);
    }

    ADCE_TEST_ASSERT(ctx.dropped_shed == 0);
    ADCE_TEST_ASSERT(ctx.admitted + ctx.dropped_limit ==
                     (uint64_t)ADCE_ENF_T_ARRIVALS);

    /* Derived from the constants rather than hardcoded, so retuning the
     * deployment knobs cannot make this assertion lie about the property. */
    elapsed_ns = (uint64_t)ADCE_ENF_T_ARRIVALS * ADCE_ENF_T_STEP_NS;
    bound = (ADCE_ENF_CAPACITY_Q16 + ADCE_ENF_RATE_Q16_PER_NS * elapsed_ns) /
            ADCE_ENF_COST_Q16;

    ADCE_TEST_ASSERT(ctx.admitted <= bound);

    /* The ceiling actually bit -- otherwise the bound above would be vacuous. */
    ADCE_TEST_ASSERT(ctx.dropped_limit > 0);
    ADCE_TEST_ASSERT(ctx.admitted < (uint64_t)ADCE_ENF_T_ARRIVALS);

    /* And it is not simply closed: the gate keeps admitting at the refill
     * rate rather than sealing shut once the initial burst is spent. */
    ADCE_TEST_ASSERT(ctx.admitted > ADCE_ENF_CAPACITY_Q16 / ADCE_ENF_COST_Q16);

    /* Time not advancing must not manufacture tokens. A repeated now_ns
     * yields zero elapsed, so once the bucket is dry the answer stays no. */
    {
        uint64_t frozen = t0 + (uint64_t)ADCE_ENF_T_ARRIVALS * ADCE_ENF_T_STEP_NS;
        uint64_t admitted_before = ctx.admitted;
        int k;

        for (k = 0; k < 1000; ++k) {
            (void)adce_enf_decide(&ctx, frozen, UINT64_MAX);
        }
        ADCE_TEST_ASSERT(ctx.admitted == admitted_before);
    }

    /* Nor may time going backwards. An unsigned wrap on (now - last) would
     * compute an enormous elapsed and refill the bucket to capacity, which is
     * a fail-OPEN triggered by a clock the gate does not control. */
    {
        uint64_t admitted_before = ctx.admitted;
        int k;

        for (k = 0; k < 1000; ++k) {
            (void)adce_enf_decide(&ctx, t0, UINT64_MAX);
        }
        ADCE_TEST_ASSERT(ctx.admitted == admitted_before);
    }

    return 0;
}

/* =====================================================================
 * 7. Determinism in (epoch contents, now_ns, draw).
 * ===================================================================== */

/* Two phases, because draining the bucket and going stale want opposite
 * things from the clock. Phase one steps time in 100 ns increments so refill
 * stays far below the arrival rate and the initial burst is actually spent;
 * phase two jumps past the advice timeout with no republication. A single
 * uniform time step cannot do both -- a step large enough to go stale refills
 * the bucket faster than arrivals can drain it, which is exactly what the
 * saw_limit assertion caught on the first attempt. */
#define ADCE_ENF_T_DENSE 12000
#define ADCE_ENF_T_LAPSED 2000
#define ADCE_ENF_T_SEQ (ADCE_ENF_T_DENSE + ADCE_ENF_T_LAPSED)
#define ADCE_ENF_T_DENSE_STEP_NS 100

static uint64_t enf_splitmix(uint64_t *s) {
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static void enf_run_sequence(adce_enf_outcome_t *out, uint64_t *stale_reads) {
    adce_enf_ctx_t ctx;
    uint64_t seed = 0x0DDBA11C0FFEE123ULL;
    const uint64_t t0 = ADCE_ADVICE_TIMEOUT_NS * 4;
    uint64_t now = t0;
    int i;

    enf_fresh(&ctx, t0);

    for (i = 0; i < ADCE_ENF_T_SEQ; ++i) {
        uint64_t draw = enf_splitmix(&seed);
        adce_q16_t pressure = (adce_q16_t)((i * 37) % (ADCE_PRESSURE_MAX + 1));

        if (i < ADCE_ENF_T_DENSE) {
            now += ADCE_ENF_T_DENSE_STEP_NS;
            if ((i % 5) == 0) {
                adce_epoch_publish(&g_enf_epoch, pressure, (uint64_t)i + 1, now);
            }
        } else if (i == ADCE_ENF_T_DENSE) {
            /* One jump past the timeout, then no further publication: the
             * remainder of the run reads stale and takes the fallback. */
            now += ADCE_ADVICE_TIMEOUT_NS + 1;
        } else {
            now += ADCE_ENF_T_DENSE_STEP_NS;
        }

        out[i] = adce_enf_decide(&ctx, now, draw);
    }

    *stale_reads = ctx.stale_reads;
}

static int test_enf_determinism(void) {
    static adce_enf_outcome_t first[ADCE_ENF_T_SEQ];
    static adce_enf_outcome_t second[ADCE_ENF_T_SEQ];
    uint64_t stale_first = 0;
    uint64_t stale_second = 0;
    int saw_admit = 0;
    int saw_shed = 0;
    int saw_limit = 0;
    int i;

    enf_run_sequence(first, &stale_first);
    enf_run_sequence(second, &stale_second);

    for (i = 0; i < ADCE_ENF_T_SEQ; ++i) {
        ADCE_TEST_ASSERT(first[i] == second[i]);

        if (first[i] == ADCE_ENF_ADMIT) { saw_admit = 1; }
        if (first[i] == ADCE_ENF_DROP_SHED) { saw_shed = 1; }
        if (first[i] == ADCE_ENF_DROP_LIMIT) { saw_limit = 1; }
    }

    /* All three outcomes and both freshness paths must appear, or "identical"
     * would be a statement about two runs that never exercised the branches. */
    ADCE_TEST_ASSERT(saw_admit == 1);
    ADCE_TEST_ASSERT(saw_shed == 1);
    ADCE_TEST_ASSERT(saw_limit == 1);
    ADCE_TEST_ASSERT(stale_first == stale_second);
    ADCE_TEST_ASSERT(stale_first >= (uint64_t)ADCE_ENF_T_LAPSED);

    return 0;
}

/* =====================================================================
 * 8. The shed fraction against a real uniform stream.
 * ===================================================================== */

#define ADCE_ENF_T_SAMPLES 1000000

static int test_enf_shed_fraction(void) {
    static const adce_q16_t pressures[] = {6554, 16384, 32768, 49152, 58982};
    size_t i;

    /* Case 1 above proves the mapping is exact over the bucket space. This
     * proves it composes correctly with a real uniform 64-bit stream, which
     * is a different claim: it is what catches a wrong shift width (draw >> 32
     * would make the fraction p/2^32, effectively zero) or an inverted
     * comparison (1 - p/65536). It deliberately cannot catch a low-bit
     * mapping, whose fraction is also correct -- case 1 covers that.
     *
     * splitmix64 rather than adce_rng_next(): the production generator's state
     * is `static _Thread_local` per translation unit, so this TU cannot seed
     * the stream an enforcement TU would use. Driving the mapping from an
     * independent, fixed-seed generator also separates what is under test --
     * the mapping -- from the quality of xorshift128+.
     *
     * Tolerance is five standard deviations of the binomial, computed per
     * pressure rather than fixed. At one million samples that is ~0.25% of the
     * sample at p = 0.5, which is narrow enough that every error class above
     * fails it by orders of magnitude, while a 5-sigma band makes the fixed
     * seed's outcome a non-issue. It is NOT tight enough to detect a one-bucket
     * offset, which moves the expectation by 15 counts against a 2500-count
     * band -- that is case 1's job, and stating the division here is the point:
     * a fraction test alone would prove very little. */
    for (i = 0; i < sizeof(pressures) / sizeof(pressures[0]); ++i) {
        adce_q16_t p = pressures[i];
        uint64_t seed = 0xC0FFEE123456789ULL + (uint64_t)i;
        double q = (double)p / (double)ADCE_PRESSURE_MAX;
        double expected = q * (double)ADCE_ENF_T_SAMPLES;
        double sigma = sqrt((double)ADCE_ENF_T_SAMPLES * q * (1.0 - q));
        double tolerance = 5.0 * sigma;
        long shed = 0;
        int k;

        for (k = 0; k < ADCE_ENF_T_SAMPLES; ++k) {
            if (adce_enf_should_shed(p, enf_splitmix(&seed))) {
                shed++;
            }
        }

        /* Floating point here is test-side tolerance arithmetic, not a drop
         * decision; the convention at adce_platform.h:381-387 constrains the
         * plane's implementation, and adce_enf_should_shed above is integer
         * throughout. */
        ADCE_TEST_ASSERT(fabs((double)shed - expected) <= tolerance);
    }

    /* The endpoints have zero variance and so are asserted exactly, not
     * within a band: a probabilistic gate that occasionally sheds at pressure
     * zero, or occasionally admits at maximal, is broken rather than noisy. */
    {
        uint64_t seed = 0x5EED5EED5EED5EEDULL;
        long shed_min = 0;
        long shed_max = 0;
        int k;

        for (k = 0; k < ADCE_ENF_T_SAMPLES; ++k) {
            uint64_t draw = enf_splitmix(&seed);
            if (adce_enf_should_shed(ADCE_PRESSURE_MIN, draw)) { shed_min++; }
            if (adce_enf_should_shed(ADCE_PRESSURE_MAX, draw)) { shed_max++; }
        }

        ADCE_TEST_ASSERT(shed_min == 0);
        ADCE_TEST_ASSERT(shed_max == ADCE_ENF_T_SAMPLES);
    }

    return 0;
}

/* =====================================================================
 * External forwarders; see the header comment.
 * ===================================================================== */

#define ADCE_ENF_TEST_EXPORT(name)                                           \
    int adce_t_##name(void);                                                 \
    int adce_t_##name(void) { return test_##name(); }

ADCE_ENF_TEST_EXPORT(enf_shed_mapping)
ADCE_ENF_TEST_EXPORT(enf_shed_monotone)
ADCE_ENF_TEST_EXPORT(enf_read_clamp)
ADCE_ENF_TEST_EXPORT(enf_stale_fallback)
ADCE_ENF_TEST_EXPORT(enf_cold_start)
ADCE_ENF_TEST_EXPORT(enf_bucket_ceiling)
ADCE_ENF_TEST_EXPORT(enf_determinism)
ADCE_ENF_TEST_EXPORT(enf_shed_fraction)
