/* Per-arrival latency of the gate.
 *
 * docs/enforcement-plane.md section 5 lists this as a harness deliverable --
 * "the clock read, the seqlock read... Asserted nowhere; measured here" -- and
 * until this file nothing measured it. The gate runs on the hot path of every
 * arrival, so its cost was the one number about the enforcement plane that was
 * pure assertion.
 *
 * This file MEASURES AND REPORTS. It never fails on a number. A latency
 * threshold on a shared CI runner is a flake generator, and this is the fast
 * per-edit gate; the only assertions here are structural -- that each fixture
 * drove the path it claims to have driven -- and those hold on any host at any
 * speed. See the note at the bottom for the bound that WOULD be worth
 * asserting, proposed rather than folded in.
 *
 * The runner and main() live in t_adce_platform.c. The case is static so
 * scripts/verify.sh's ran-tests guard finds it in the source, with one external
 * forwarder at the bottom.
 *
 * =====================================================================
 * SEPARATING THE GATE FROM THE CLOCK READ
 * =====================================================================
 *
 * There is nothing to subtract, because the separation is structural and
 * already exists in the API:
 *
 *     adce_enf_admit(adce_enf_ctx_t *ctx, uint64_t now_ns)
 *
 * now_ns is a PARAMETER. Neither adce_enf_admit nor adce_enf_decide calls
 * adce_now_ns; the clock read belongs to the ingress site, step 1 of the recipe
 * in enforcement-plane.md section 3. So timing adce_enf_admit times the gate
 * and only the gate, and the doc's 20-25 ns vDSO estimate never enters the
 * number.
 *
 * That estimate is not taken on trust either. adce_now_ns is measured here with
 * the same instrument, so the per-arrival ingress cost can be stated as two
 * MEASURED numbers -- clock plus gate -- rather than one measured and one
 * assumed. Whether the clock dominates is then an observation rather than a
 * prediction.
 *
 * =====================================================================
 * WHY TWO INSTRUMENTS
 * =====================================================================
 *
 * The gate costs single-digit nanoseconds and the only clock available to
 * measure it is coarser than that: CLOCK_MONOTONIC_RAW quantises to the host's
 * tick, ~41.7 ns on Apple Silicon. One clock pair around one call therefore
 * cannot resolve a median -- it reports the quantisation, not the work.
 *
 * So there are two, and each answers the question the other cannot:
 *
 *   BATCH  -- one clock pair around LAT_BATCH_CALLS calls, cost divided out.
 *             Amortises the clock away entirely and resolves the CENTRAL cost
 *             to picoseconds. It cannot see a tail: one slow call inside a
 *             batch is diluted by the other 1023.
 *
 *   PAIRED -- one clock pair around ONE call. Its floor is the quantisation,
 *             so its median is meaningless; but a call that takes microseconds
 *             -- a seqlock retry, a preemption, a cache miss on the epoch line
 *             -- shows up whole. This is the instrument for p99 and max, which
 *             on a hot path is the half that actually matters.
 *
 * PAIRED is reported against an EMPTY-PAIR baseline: two clock reads with
 * nothing between them, sampled the same number of times. That is the
 * instrument's own distribution. The gate's paired numbers are only meaningful
 * as a shift above that floor, and printing the floor next to them is what
 * stops a reader from mistaking the instrument for the subject.
 *
 * WHAT THE BATCH NUMBER IS NOT. It is a LOWER BOUND on real per-arrival cost.
 * A tight loop keeps ctx and the epoch cache line in L1 and the branch
 * predictors perfectly trained, which is the best case, not the typical one. A
 * real ingress site interleaves request work between arrivals and will
 * sometimes take a cold line. The number below is "what the gate costs when
 * nothing is in its way", and it should be read that way.
 */

#include "../include/adce_enforce.h"

#include <stdio.h>
#include <stdlib.h>

#define ADCE_TEST_ASSERT(cond)                                               \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
            return 1;                                                        \
        }                                                                    \
    } while (0)

/* Which profile produced the numbers. This file is compiled into all three, and
 * a sanitized build's latency is an artifact of the sanitizer rather than a
 * property of the gate -- ASan redzones every load and TSan intercepts every
 * atomic, so the seqlock read alone is inflated by more than an order of
 * magnitude. Printing the profile is what keeps a TSan number from ever being
 * quoted as the gate's cost.
 *
 * Nested rather than a single && expression: __has_feature is Clang-only, and
 * on GCC the preprocessor would try to evaluate the call as an identifier
 * followed by parentheses and reject the line outright. */
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define ADCE_LAT_PROFILE "asan+ubsan"
#  elif __has_feature(thread_sanitizer)
#    define ADCE_LAT_PROFILE "tsan"
#  endif
#endif
#if !defined(ADCE_LAT_PROFILE)
#  if defined(__SANITIZE_ADDRESS__)
#    define ADCE_LAT_PROFILE "asan+ubsan"
#  elif defined(__SANITIZE_THREAD__)
#    define ADCE_LAT_PROFILE "tsan"
#  else
#    define ADCE_LAT_PROFILE "strict-O2"
#  endif
#endif

/* Sample counts. Chosen so p99 is an order statistic with real samples behind
 * it -- 16384 paired samples put 164 above the p99 mark -- while the whole case
 * stays inside a fraction of a second even under TSan, because it runs in the
 * per-edit loop and ADCE_REPEAT multiplies it. */
#define LAT_BATCH_SAMPLES ((size_t)256)
#define LAT_BATCH_CALLS ((uint64_t)1024)
#define LAT_PAIR_SAMPLES ((size_t)16384)

/* Ahead of any sampling: forces lazy RNG seeding, trains the branch predictors
 * and pulls both cache lines in, so first-call costs land here rather than in
 * the distribution. Counted in the expected totals below rather than discarded,
 * so the structural assertions still account for every call made. */
#define LAT_WARMUP_CALLS ((uint64_t)2048)

/* LAT_BATCH_CALLS must fit inside one bucket fill, or a batch measuring admits
 * would silently start measuring DROP_LIMIT partway through -- the outcome
 * assertions below would catch it, but the constraint is worth stating where
 * the constant is chosen rather than only where it is checked. */
_Static_assert(LAT_BATCH_CALLS <= ADCE_ENF_CAPACITY_Q16 / ADCE_ENF_COST_Q16,
               "a batch of admits must fit in one bucket fill");
_Static_assert(LAT_WARMUP_CALLS <= ADCE_ENF_CAPACITY_Q16 / ADCE_ENF_COST_Q16,
               "the warmup must fit in one bucket fill");

#define LAT_TOTAL_CALLS                                                      \
    (LAT_WARMUP_CALLS + (uint64_t)LAT_BATCH_SAMPLES * LAT_BATCH_CALLS +      \
     (uint64_t)LAT_PAIR_SAMPLES)

/* Static rather than malloc'd: no allocation anywhere in this project's test
 * binary, so LeakSanitizer has nothing to weigh in on. */
static uint64_t g_lat_batch_ps[LAT_BATCH_SAMPLES];
static uint64_t g_lat_pair_ns[LAT_PAIR_SAMPLES];
static uint64_t g_lat_base_ns[LAT_PAIR_SAMPLES];

static adce_epoch_state_t g_lat_epoch;

/* Keeps every measured return value live. Without a consumer the optimiser is
 * free to drop the returns, and while the ctx side effects mean the calls
 * themselves survive, the comparison chain that produces the outcome need not.
 * volatile is the cheapest way to say "this result leaves the program". */
static volatile uint64_t g_lat_sink;

/* =====================================================================
 * Order statistics.
 * ===================================================================== */

static int lat_cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    /* Not a subtraction: the difference of two uint64_t nanosecond counts does
     * not fit in the int this comparator must return. */
    if (x < y) { return -1; }
    if (x > y) { return 1; }
    return 0;
}

typedef struct {
    uint64_t p50;
    uint64_t p99;
    uint64_t max;
} lat_dist_t;

static lat_dist_t lat_summarize(uint64_t *v, size_t n) {
    lat_dist_t d;
    qsort(v, n, sizeof(v[0]), lat_cmp_u64);
    d.p50 = v[n / 2];
    d.p99 = v[(n * 99) / 100];
    d.max = v[n - 1];
    return d;
}

/* Picoseconds as N.NNN ns. Integer only: the Q16 lane's no-floating-point
 * convention does not bind a test file, but printf("%f") on a value this small
 * would invite reading three decimals of noise as three decimals of signal. */
static void lat_print_ps(uint64_t ps) {
    printf("%llu.%03llu", (unsigned long long)(ps / 1000),
           (unsigned long long)(ps % 1000));
}

/* =====================================================================
 * The three fixtures.
 *
 * All three freeze now_ns at T and publish the epoch with observed_at_ns == T.
 * Two consequences, both load-bearing:
 *
 *   - now_ns - observed_at_ns is 0 on every call, so adce_epoch_is_stale is
 *     never true and the gate always takes the published-value branch. Wall
 *     time passing during the run cannot flip the fixture, because the run
 *     never consults the wall clock for the decision.
 *   - elapsed_ns is 0 on every refill, so the bucket never regains a token on
 *     its own. What the bucket holds is exactly what the fixture put there.
 *
 * Nothing here stubs the RNG. adce_enf_admit draws from this translation
 * unit's real adce_rng_tls on every call -- the real path -- and each fixture
 * is built so the draw provably cannot change the outcome.
 * ===================================================================== */

typedef enum {
    LAT_FIX_ADMIT = 0,
    LAT_FIX_SHED = 1,
    LAT_FIX_LIMIT = 2
} lat_fixture_t;

static const char *lat_fixture_name(lat_fixture_t f) {
    switch (f) {
        case LAT_FIX_ADMIT: return "admit";
        case LAT_FIX_SHED: return "shed ";
        case LAT_FIX_LIMIT: return "limit";
    }
    return "?????";
}

static void lat_open(lat_fixture_t fix, adce_enf_ctx_t *ctx, uint64_t t) {
    memset(&g_lat_epoch, 0, sizeof(g_lat_epoch));

    /* SHED: at ADCE_PRESSURE_MAX the test is (draw >> 48) < 65536, true for
     * every one of the 65536 buckets, so every arrival sheds regardless of what
     * the stream produces. This is the SHORTEST path -- seqlock read, clamp,
     * shed test, return -- and it never reaches the bucket.
     *
     * ADMIT and LIMIT: at ADCE_PRESSURE_MIN the test is (draw >> 48) < 0, false
     * for every draw, so shedding is off and the bucket alone decides. */
    adce_epoch_publish(&g_lat_epoch, fix == LAT_FIX_SHED ? ADCE_PRESSURE_MAX
                                                         : ADCE_PRESSURE_MIN,
                       1, t);

    adce_enf_thread_init(ctx, &g_lat_epoch, t);

    /* LIMIT: an empty bucket that can never refill, so try_take fails on every
     * call. This is the LONGEST failing path -- everything ADMIT does except
     * the successful take. Writing the field directly rather than draining it
     * with 4096 calls keeps the warmup count honest; the field belongs to a
     * ctx this file owns, and no decision logic is touched. */
    if (fix == LAT_FIX_LIMIT) {
        ctx->tokens_q16 = 0;
    }
}

/* ADMIT is the only fixture that consumes state as it measures: each admitted
 * arrival takes a token and elapsed_ns is 0, so nothing gives it back. Topping
 * the bucket up keeps every sampled call an admit.
 *
 * Always called OUTSIDE the clock pair. It is a store to a field the gate is
 * about to write anyway, on a line already in L1, so it does not colour what
 * the pair sees. */
static void lat_replenish(lat_fixture_t fix, adce_enf_ctx_t *ctx) {
    if (fix == LAT_FIX_ADMIT) {
        ctx->tokens_q16 = ADCE_ENF_CAPACITY_Q16;
    }
}

/* Every call this fixture made landed on the outcome it was built for. This is
 * the assertion that makes the numbers mean anything: a measurement of the
 * wrong path is worse than no measurement, because it looks like data. It
 * constrains outcomes and never time, so it cannot flake on a slow host. */
static int lat_check_fixture(lat_fixture_t fix, const adce_enf_ctx_t *ctx) {
    uint64_t hit = fix == LAT_FIX_ADMIT   ? ctx->admitted
                   : fix == LAT_FIX_SHED  ? ctx->dropped_shed
                                          : ctx->dropped_limit;

    ADCE_TEST_ASSERT(hit == LAT_TOTAL_CALLS);
    ADCE_TEST_ASSERT(ctx->admitted + ctx->dropped_shed + ctx->dropped_limit ==
                     LAT_TOTAL_CALLS);
    /* Never stale, so the seqlock read succeeded every time and the pressure
     * measured was the published one, not ADCE_ENF_STALE_PRESSURE. A single
     * stale read would mean part of the run measured a different branch. */
    ADCE_TEST_ASSERT(ctx->stale_reads == 0);
    return 0;
}

/* =====================================================================
 * The measurement.
 * ===================================================================== */

static void lat_measure(lat_fixture_t fix, adce_enf_ctx_t *ctx, uint64_t t) {
    uint64_t sum = 0;
    uint64_t k;
    size_t s;

    for (k = 0; k < LAT_WARMUP_CALLS; ++k) {
        sum += (uint64_t)adce_enf_admit(ctx, t);
    }
    lat_replenish(fix, ctx);

    for (s = 0; s < LAT_BATCH_SAMPLES; ++s) {
        uint64_t t0, t1;

        lat_replenish(fix, ctx);

        /* clock_gettime is an opaque external call, which makes it a compiler
         * barrier: the loop cannot be hoisted across either read. No inline asm
         * is needed to fence this, and none is used -- arch-conditional code is
         * confined to one block in adce_platform.h by a locked decision. */
        t0 = adce_now_ns();
        for (k = 0; k < LAT_BATCH_CALLS; ++k) {
            sum += (uint64_t)adce_enf_admit(ctx, t);
        }
        t1 = adce_now_ns();

        g_lat_batch_ps[s] = ((t1 - t0) * 1000ULL) / LAT_BATCH_CALLS;
    }

    for (s = 0; s < LAT_PAIR_SAMPLES; ++s) {
        uint64_t t0, t1;

        lat_replenish(fix, ctx);

        t0 = adce_now_ns();
        sum += (uint64_t)adce_enf_admit(ctx, t);
        t1 = adce_now_ns();

        g_lat_pair_ns[s] = t1 - t0;
    }

    g_lat_sink += sum;
}

/* The instrument measuring itself: two clock reads, nothing between. Every
 * paired number above is this plus the gate, so this is the floor they must be
 * read against. */
static void lat_measure_floor(void) {
    size_t s;

    for (s = 0; s < LAT_PAIR_SAMPLES; ++s) {
        uint64_t t0 = adce_now_ns();
        uint64_t t1 = adce_now_ns();
        g_lat_base_ns[s] = t1 - t0;
    }
}

/* adce_now_ns by the batch instrument, so the other half of the per-arrival
 * cost is a measured number on this host rather than the doc's 20-25 ns
 * estimate for a Linux vDSO. */
static uint64_t lat_measure_clock_ps(void) {
    uint64_t sum = 0;
    uint64_t k;
    size_t s;

    for (s = 0; s < LAT_BATCH_SAMPLES; ++s) {
        uint64_t t0 = adce_now_ns();
        for (k = 0; k < LAT_BATCH_CALLS; ++k) {
            sum += adce_now_ns();
        }
        uint64_t t1 = adce_now_ns();
        g_lat_batch_ps[s] = ((t1 - t0) * 1000ULL) / LAT_BATCH_CALLS;
    }

    g_lat_sink += sum;
    return lat_summarize(g_lat_batch_ps, LAT_BATCH_SAMPLES).p50;
}

static int test_latency_per_arrival(void) {
    static const lat_fixture_t fixtures[] = {LAT_FIX_ADMIT, LAT_FIX_SHED,
                                             LAT_FIX_LIMIT};
    uint64_t t = adce_now_ns();
    uint64_t clock_ps;
    lat_dist_t floor_ns;
    size_t i;

    /* No host string: scripts/verify.sh already prints uname once per run, and
     * inventing a platform-name macro to repeat it here would add a symbol the
     * codebase does not have. ADCE_CACHELINE is the existing value that says
     * which arch branch compiled -- 64 on x86_64, 128 on arm64 -- which is the
     * part that bears on the seqlock read being timed. */
    printf("  LATENCY profile=%s cacheline=%zu calls/outcome=%llu"
           " batch=%llux%llu paired=%llu\n",
           ADCE_LAT_PROFILE, ADCE_CACHELINE,
           (unsigned long long)LAT_TOTAL_CALLS,
           (unsigned long long)LAT_BATCH_SAMPLES,
           (unsigned long long)LAT_BATCH_CALLS,
           (unsigned long long)LAT_PAIR_SAMPLES);

    lat_measure_floor();
    floor_ns = lat_summarize(g_lat_base_ns, LAT_PAIR_SAMPLES);
    clock_ps = lat_measure_clock_ps();

    printf("  LATENCY clock adce_now_ns          batch p50=");
    lat_print_ps(clock_ps);
    printf("ns  <- the OTHER half of per-arrival cost, not part of the gate\n");

    printf("  LATENCY floor empty clock pair     paired p50=%lluns p99=%lluns"
           " max=%lluns  <- the instrument, not the gate\n",
           (unsigned long long)floor_ns.p50, (unsigned long long)floor_ns.p99,
           (unsigned long long)floor_ns.max);

    for (i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); ++i) {
        lat_fixture_t fix = fixtures[i];
        adce_enf_ctx_t ctx;
        lat_dist_t batch;
        lat_dist_t pair;

        lat_open(fix, &ctx, t);
        lat_measure(fix, &ctx, t);

        /* Before printing anything: if the fixture did not drive its outcome,
         * the numbers describe some other path and must not be reported at
         * all. */
        ADCE_TEST_ASSERT(lat_check_fixture(fix, &ctx) == 0);

        batch = lat_summarize(g_lat_batch_ps, LAT_BATCH_SAMPLES);
        pair = lat_summarize(g_lat_pair_ns, LAT_PAIR_SAMPLES);

        printf("  LATENCY gate  adce_enf_admit %s batch p50=",
               lat_fixture_name(fix));
        lat_print_ps(batch.p50);
        printf("ns p99=");
        lat_print_ps(batch.p99);
        printf("ns max=");
        lat_print_ps(batch.max);
        printf("ns | paired p50=%lluns p99=%lluns max=%lluns\n",
               (unsigned long long)pair.p50, (unsigned long long)pair.p99,
               (unsigned long long)pair.max);
    }

    /* Consumed so the sink is not merely written. */
    ADCE_TEST_ASSERT(g_lat_sink != UINT64_MAX);

    /* No latency assertion, by design. See the file header: this reports.
     *
     * THE BOUND THAT WOULD BE WORTH ASSERTING, proposed here and deliberately
     * not implemented, because it is a different kind of check and belongs in
     * its own step: the gate must make zero allocations and zero syscalls per
     * arrival. That is structural rather than temporal -- it is a statement
     * about which code exists on the path, not about how fast this host ran it
     * -- so it holds identically on a loaded CI runner and a quiet laptop, and
     * it cannot flake. It is also the property that actually protects the
     * numbers above: latency drifting from 4 ns to 6 ns is noise, whereas a
     * malloc or a futex appearing on the arrival path is a design regression
     * that no timing threshold would reliably catch, since a slow host hides it
     * inside the variance. Enforcing it needs an interposer for the allocator
     * and a syscall counter, neither of which exists in this repository yet. */
    return 0;
}

/* =====================================================================
 * External forwarder; see the header comment.
 * ===================================================================== */

int adce_t_latency_per_arrival(void);
int adce_t_latency_per_arrival(void) { return test_latency_per_arrival(); }
