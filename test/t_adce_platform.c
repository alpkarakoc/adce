#include "../include/adce_platform.h"

#include <pthread.h>
#include <stdio.h>

#define ADCE_TEST_ASSERT(cond)                                               \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
            return 1;                                                        \
        }                                                                    \
    } while (0)

static int test_q16_arithmetic(void) {
    ADCE_TEST_ASSERT(adce_q16_from_int(1) == ADCE_Q16_ONE);
    ADCE_TEST_ASSERT(adce_q16_to_int(adce_q16_from_int(42)) == 42);
    ADCE_TEST_ASSERT(adce_q16_to_int(adce_q16_from_int(-7)) == -7);

    adce_q16_t three = adce_q16_from_int(3);
    adce_q16_t four = adce_q16_from_int(4);

    ADCE_TEST_ASSERT(adce_q16_to_int(adce_q16_add(three, four)) == 7);
    ADCE_TEST_ASSERT(adce_q16_to_int(adce_q16_sub(four, three)) == 1);
    ADCE_TEST_ASSERT(adce_q16_to_int(adce_q16_mul(three, four)) == 12);
    ADCE_TEST_ASSERT(adce_q16_to_int(adce_q16_div(adce_q16_from_int(12), four)) == 3);

    adce_q16_t half = adce_q16_div(ADCE_Q16_ONE, adce_q16_from_int(2));
    ADCE_TEST_ASSERT(half == ADCE_Q16_ONE / 2);

    ADCE_TEST_ASSERT(adce_q16_min(three, four) == three);
    ADCE_TEST_ASSERT(adce_q16_max(three, four) == four);

    /* 1,000,000 * 1,000,000 = 10^12: the raw Q16.16 operands, multiplied
     * directly as int64_t before shifting, would overflow int64_t. Routing
     * through the __int128 intermediate must still land on the exact
     * result. */
    adce_q16_t big = adce_q16_from_int(1000000);
    adce_q16_t product = adce_q16_mul(big, big);
    adce_q16_t expected = (adce_q16_t)1000000000000LL << ADCE_Q16_FRAC_BITS;
    ADCE_TEST_ASSERT(product == expected);

    return 0;
}

static int test_q16_boundaries(void) {
    /* Negative operands. adce_q16_from_int must not left-shift a negative
     * value (C11 6.5.7p4); UBSan fails the second profile if it does. */
    ADCE_TEST_ASSERT(adce_q16_from_int(-1) == -ADCE_Q16_ONE);
    ADCE_TEST_ASSERT(adce_q16_from_int(-7) == -7 * ADCE_Q16_ONE);
    ADCE_TEST_ASSERT(adce_q16_to_int(adce_q16_from_int(-7)) == -7);

    /* INT32_MAX and INT32_MIN are the widest inputs the conversion accepts,
     * so they are the largest and smallest values that can round-trip. Both
     * must come back exact, not merely close. */
    ADCE_TEST_ASSERT(adce_q16_to_int(adce_q16_from_int(INT32_MAX)) == INT32_MAX);
    ADCE_TEST_ASSERT(adce_q16_to_int(adce_q16_from_int(INT32_MIN)) == INT32_MIN);
    ADCE_TEST_ASSERT(adce_q16_from_int(INT32_MAX) ==
                     ((adce_q16_t)INT32_MAX << ADCE_Q16_FRAC_BITS));
    ADCE_TEST_ASSERT(adce_q16_from_int(INT32_MIN) == -((adce_q16_t)1 << 47));

    /* Exact negative division: the remainder is zero, so the quotient is
     * already floored and the correction must stay off. */
    ADCE_TEST_ASSERT(adce_q16_to_int(adce_q16_mul(adce_q16_from_int(-3),
                                                  adce_q16_from_int(4))) == -12);
    ADCE_TEST_ASSERT(adce_q16_to_int(adce_q16_div(adce_q16_from_int(-12),
                                                  adce_q16_from_int(4))) == -3);
    ADCE_TEST_ASSERT(adce_q16_to_int(adce_q16_div(adce_q16_from_int(12),
                                                  adce_q16_from_int(-4))) == -3);

    /* Negative counterpart of the 10^12 overflow case in test_q16_arithmetic:
     * the __int128 intermediate must carry it without wrapping. */
    ADCE_TEST_ASSERT(adce_q16_mul(adce_q16_from_int(-1000000),
                                  adce_q16_from_int(1000000)) ==
                     -((adce_q16_t)1000000000000LL << ADCE_Q16_FRAC_BITS));

    /* A zero divisor saturates toward the numerator's sign rather than
     * executing a division by zero. */
    ADCE_TEST_ASSERT(adce_q16_div(adce_q16_from_int(1), 0) == ADCE_Q16_MAX);
    ADCE_TEST_ASSERT(adce_q16_div(adce_q16_from_int(-1), 0) == ADCE_Q16_MIN);
    ADCE_TEST_ASSERT(adce_q16_div(0, 0) == ADCE_Q16_MAX);

    /* ADCE_Q16_MIN / -1 is +2^79, which no 64-bit lane holds: it must clamp
     * to the maximum instead of narrowing silently. The division is exact, so
     * the floor correction does not fire and cannot disturb the clamp. */
    ADCE_TEST_ASSERT(adce_q16_div(ADCE_Q16_MIN, -1) == ADCE_Q16_MAX);
    ADCE_TEST_ASSERT(adce_q16_div(ADCE_Q16_MAX, -1) == ADCE_Q16_MIN);

    ADCE_TEST_ASSERT(adce_q16_div(adce_q16_from_int(-12),
                                  adce_q16_from_int(-4)) == 3 * ADCE_Q16_ONE);

    /* Half-integers ARE representable in Q16.16, so these four are exact and
     * must come back unchanged -- they would read identically under either
     * rounding mode. They are here to catch a correction applied where no
     * remainder exists, which is the likeliest way to get flooring wrong. */
    ADCE_TEST_ASSERT(adce_q16_div(adce_q16_from_int(-7),
                                  adce_q16_from_int(2)) == -7 * ADCE_Q16_ONE / 2);
    ADCE_TEST_ASSERT(adce_q16_div(adce_q16_from_int(7),
                                  adce_q16_from_int(-2)) == -7 * ADCE_Q16_ONE / 2);
    ADCE_TEST_ASSERT(adce_q16_div(adce_q16_from_int(-1),
                                  adce_q16_from_int(2)) == -ADCE_Q16_ONE / 2);
    ADCE_TEST_ASSERT(adce_q16_div(adce_q16_from_int(1),
                                  adce_q16_from_int(-2)) == -ADCE_Q16_ONE / 2);

    /* Thirds are NOT representable, so these are the cases the correction
     * exists for and the only ones whose value changes. Truncation toward
     * zero returned -21845 and -152917; flooring returns one less. Same-sign
     * operands round the same way under both modes and must not shift. */
    ADCE_TEST_ASSERT(adce_q16_div(adce_q16_from_int(-1),
                                  adce_q16_from_int(3)) == -21846);
    ADCE_TEST_ASSERT(adce_q16_div(adce_q16_from_int(1),
                                  adce_q16_from_int(-3)) == -21846);
    ADCE_TEST_ASSERT(adce_q16_div(adce_q16_from_int(-7),
                                  adce_q16_from_int(3)) == -152918);
    ADCE_TEST_ASSERT(adce_q16_div(adce_q16_from_int(1),
                                  adce_q16_from_int(3)) == 21845);
    ADCE_TEST_ASSERT(adce_q16_div(adce_q16_from_int(-1),
                                  adce_q16_from_int(-3)) == 21845);

    /* adce_q16_to_int and adce_q16_div now agree: both round toward negative
     * infinity, so composing them gives integer floor division. -7/2 lands on
     * -4, not the -3 that truncation toward zero would produce. */
    ADCE_TEST_ASSERT(adce_q16_to_int(adce_q16_div(adce_q16_from_int(-7),
                                                  adce_q16_from_int(2))) == -4);
    ADCE_TEST_ASSERT(adce_q16_to_int(adce_q16_div(adce_q16_from_int(-1),
                                                  adce_q16_from_int(3))) == -1);

    /* Property sweep, checked from the defining relation rather than by
     * recomputing the quotient the same way the implementation does: for a
     * floored quotient the remainder carries the DIVISOR's sign and is
     * strictly smaller in magnitude. Truncation toward zero violates the sign
     * half of that for precisely the mixed-sign inexact pairs. */
    for (int32_t na = -9; na <= 9; ++na) {
        for (int32_t da = -9; da <= 9; ++da) {
            if (da == 0) {
                continue;
            }

            adce_q16_t quot =
                adce_q16_div(adce_q16_from_int(na), adce_q16_from_int(da));

            /* The same unsigned detour the header takes: left-shifting a
             * negative signed value is undefined and UBSan checks profile 2. */
            adce_i128_t num =
                (adce_i128_t)((adce_u128_t)(adce_i128_t)adce_q16_from_int(na)
                              << ADCE_Q16_FRAC_BITS);
            adce_i128_t den = (adce_i128_t)adce_q16_from_int(da);
            adce_i128_t rem = num - (adce_i128_t)quot * den;

            ADCE_TEST_ASSERT(rem == 0 || ((rem < 0) == (den < 0)));
            ADCE_TEST_ASSERT((rem < 0 ? -rem : rem) < (den < 0 ? -den : den));
        }
    }

    return 0;
}

static int test_token_bucket(void) {
    uint64_t capacity = (uint64_t)ADCE_Q16_ONE * 1000;
    uint64_t rate = (uint64_t)ADCE_Q16_ONE; /* 1 token (Q16) per ns */

    uint64_t tokens = 0;
    tokens = adce_token_refill(tokens, rate, 10, capacity);
    ADCE_TEST_ASSERT(tokens == (uint64_t)ADCE_Q16_ONE * 10);

    /* Refill must clamp at capacity rather than wrap. */
    tokens = adce_token_refill(tokens, rate, UINT64_MAX, capacity);
    ADCE_TEST_ASSERT(tokens == capacity);

    ADCE_TEST_ASSERT(adce_token_try_take(&tokens, capacity / 2) == 1);
    ADCE_TEST_ASSERT(tokens == capacity / 2);

    ADCE_TEST_ASSERT(adce_token_try_take(&tokens, capacity) == 0);
    ADCE_TEST_ASSERT(tokens == capacity / 2);

    return 0;
}

static int test_rng(void) {
    uint64_t a = adce_rng_next();
    uint64_t b = adce_rng_next();
    uint64_t c = adce_rng_next();

    ADCE_TEST_ASSERT(a != b || b != c);
    ADCE_TEST_ASSERT(adce_rng_tls.initialized == 1);

    for (int i = 0; i < 10000; ++i) {
        double u = adce_rng_next_unit();
        ADCE_TEST_ASSERT(u >= 0.0 && u < 1.0);
    }

    return 0;
}

/* The kernel entropy syscall behind ADCE_GET_ENTROPY, executed and gated.
 *
 * WHY THIS EXISTS, and it is narrower than "the Darwin arm never runs". That
 * arm DOES run on this host: adce_rng_seed calls ADCE_GET_ENTROPY on first use
 * of the RNG, so every gate execution on Darwin already enters the function.
 * What no shipping path reaches is the CHUNKING, and its own comment calls it
 * mandatory rather than defensive:
 *
 *     getentropy() is all-or-nothing per call but refuses any request over 256
 *     bytes, so the chunking loop is mandatory, not defensive.
 *
 * The only shipping caller asks for sizeof(buf) == 16 bytes. 16 <= 256, so the
 * clamp `if (chunk > 256U) chunk = 256U;` has never been taken by any caller on
 * any host, and the loop has never iterated more than once. A defect in that
 * arithmetic -- an off-by-one, a chunk that never advances, a second chunk left
 * unwritten -- is invisible to the whole suite and to production.
 *
 * This case is deliberately PLATFORM-AGNOSTIC. It names neither __APPLE__ nor
 * __linux__ and carries no #ifdef, so it exercises whichever arm the host
 * compiled. On Darwin that drives the chunking loop for real; on Linux it
 * drives getrandom at the same sizes. What it does NOT claim to cover is the
 * Linux arm's EINTR and short-read retries, which need a signal to provoke and
 * are untouched here.
 *
 * FOUR PROPERTIES, and only the first is a smoke test:
 *
 *   1. the call reports success;
 *   2. every byte inside the request is written -- checked PER 256-BYTE REGION,
 *      so a loop that fills the first chunk and stops is caught where a
 *      whole-buffer check would pass on the first chunk alone;
 *   3. no byte past the request is touched, which is what catches an off-by-one
 *      in the chunk arithmetic;
 *   4. two successive fills differ, so a stub returning 0 without writing
 *      cannot pass.
 *
 * ON THE POISON TEST AND ITS ONE ASSUMPTION. "Written" is inferred from the
 * poison byte being gone, and genuine entropy can legitimately produce the
 * poison value. Per 16-byte region the odds of an all-poison result are
 * 256^-16, about 1e-39. That is a probabilistic argument and it is stated
 * rather than hidden; the regions checked are never smaller than 16 bytes. */
#define ENTROPY_POISON 0xA5u
#define ENTROPY_GUARD  0x5Au
#define ENTROPY_MAX    1024u

/* Returns 1 when at least one byte in [lo,hi) differs from the poison. */
static int entropy_region_written(const uint8_t *b, size_t lo, size_t hi) {
    size_t i;
    for (i = lo; i < hi; ++i) {
        if (b[i] != (uint8_t)ENTROPY_POISON) {
            return 1;
        }
    }
    return 0;
}

static int test_platform_entropy(void) {
    /* Lengths chosen around the 256-byte getentropy limit: below it, exactly
     * on it, one past it -- which is the smallest input that forces a second
     * chunk -- and well past it, which forces four. */
    static const size_t lens[] = {16u, 255u, 256u, 257u, 512u, 1000u};
    static uint8_t buf[ENTROPY_MAX];
    static uint8_t prev[ENTROPY_MAX];
    size_t k;

    for (k = 0; k < sizeof(lens) / sizeof(lens[0]); ++k) {
        size_t len = lens[k];
        size_t off;
        int rc;

        memset(buf, ENTROPY_POISON, sizeof(buf));
        memset(buf + len, ENTROPY_GUARD, sizeof(buf) - len);

        rc = ADCE_GET_ENTROPY(buf, len);
        if (rc != 0) {
            fprintf(stderr,
                    "FAIL: entropy len=%zu returned %d, expected 0 -- the"
                    " kernel entropy syscall behind ADCE_GET_ENTROPY failed\n",
                    len, rc);
            return 1;
        }

        /* Property 2, per region rather than per buffer. */
        for (off = 0; off < len; off += 256u) {
            size_t end = (off + 256u < len) ? off + 256u : len;
            if (end - off < 16u) {
                continue; /* too short for the 256^-16 argument above */
            }
            if (!entropy_region_written(buf, off, end)) {
                fprintf(stderr,
                        "FAIL: entropy len=%zu left bytes [%zu,%zu) entirely"
                        " unwritten -- the chunking loop did not cover the"
                        " whole request\n",
                        len, off, end);
                return 1;
            }
        }

        /* Property 3: nothing past the request moved. */
        for (off = len; off < sizeof(buf); ++off) {
            if (buf[off] != (uint8_t)ENTROPY_GUARD) {
                fprintf(stderr,
                        "FAIL: entropy len=%zu wrote past the request at"
                        " offset %zu (0x%02X, expected 0x%02X) -- off-by-one"
                        " in the chunk arithmetic\n",
                        len, off, buf[off], (unsigned)ENTROPY_GUARD);
                return 1;
            }
        }

        /* Property 4: a second fill differs from the first. */
        if (k > 0 && len == lens[k - 1]) {
            /* unreachable with the table above; kept honest if it changes */
            ADCE_TEST_ASSERT(memcmp(buf, prev, len) != 0);
        }
        rc = ADCE_GET_ENTROPY(prev, len);
        if (rc != 0) {
            fprintf(stderr, "FAIL: entropy len=%zu second call returned %d\n",
                    len, rc);
            return 1;
        }
        if (memcmp(buf, prev, len) == 0) {
            fprintf(stderr,
                    "FAIL: entropy len=%zu produced identical bytes twice --"
                    " the source is not returning entropy\n",
                    len);
            return 1;
        }
    }

    printf("  ENTROPY ADCE_GET_ENTROPY executed at %zu lengths up to %u bytes;"
           " chunking exercised above 256\n",
           sizeof(lens) / sizeof(lens[0]), (unsigned)lens[5]);
    return 0;
}

static int test_time_source(void) {
    uint64_t t1 = adce_now_ns();
    uint64_t t2 = adce_now_ns();
    ADCE_TEST_ASSERT(t2 >= t1);
    return 0;
}

static int test_epoch_state_lock_free(void) {
    static adce_epoch_state_t state;
    memset(&state, 0, sizeof(state));

    /* A non-lock-free _Atomic is implemented with a hidden lock, which would
     * put a mutex on the publication path. The header's size and alignment
     * assertions cannot see that -- a lock-backed atomic can still be
     * 8 bytes wide -- so it is checked here, on the real struct members
     * rather than on the underlying types. */
    ADCE_TEST_ASSERT(atomic_is_lock_free(&state.sequence));
    ADCE_TEST_ASSERT(atomic_is_lock_free(&state.pressure));
    ADCE_TEST_ASSERT(atomic_is_lock_free(&state.epoch_id));
    ADCE_TEST_ASSERT(atomic_is_lock_free(&state.observed_at_ns));

    return 0;
}

static int test_seqlock_single_threaded(void) {
    static adce_epoch_state_t state;
    memset(&state, 0, sizeof(state));

    adce_epoch_publish(&state, adce_q16_from_int(5), 1, 1000);

    adce_q16_t pressure;
    uint64_t epoch_id;
    uint64_t observed_at_ns;

    ADCE_TEST_ASSERT(adce_epoch_read(&state, &pressure, &epoch_id, &observed_at_ns) == 1);
    ADCE_TEST_ASSERT(pressure == adce_q16_from_int(5));
    ADCE_TEST_ASSERT(epoch_id == 1);
    ADCE_TEST_ASSERT(observed_at_ns == 1000);

    ADCE_TEST_ASSERT(adce_epoch_is_stale(1000, 1000) == 0);
    ADCE_TEST_ASSERT(adce_epoch_is_stale(1000, 1000 + ADCE_ADVICE_TIMEOUT_NS + 1) == 1);

    return 0;
}

#define ADCE_STRESS_ITERATIONS 200000

static adce_epoch_state_t g_stress_state;
static _Atomic int g_stress_stop;
static _Atomic int g_stress_failed;

static void *stress_writer(void *arg) {
    (void)arg;
    for (uint64_t i = 1; i <= ADCE_STRESS_ITERATIONS; ++i) {
        adce_epoch_publish(&g_stress_state, adce_q16_from_int((int32_t)(i % 1000)), i, i * 10);
    }
    atomic_store_explicit(&g_stress_stop, 1, memory_order_release);
    return NULL;
}

static void *stress_reader(void *arg) {
    (void)arg;
    adce_q16_t pressure;
    uint64_t epoch_id;
    uint64_t observed_at_ns;

    while (!atomic_load_explicit(&g_stress_stop, memory_order_acquire)) {
        if (adce_epoch_read(&g_stress_state, &pressure, &epoch_id, &observed_at_ns)) {
            /* Fields are published atomically together: this relation must
             * always hold for any successfully read (non-torn) snapshot. */
            if (observed_at_ns != epoch_id * 10 ||
                pressure != adce_q16_from_int((int32_t)(epoch_id % 1000))) {
                atomic_store_explicit(&g_stress_failed, 1, memory_order_release);
                return NULL;
            }
        }
    }
    return NULL;
}

static int test_seqlock_concurrent(void) {
    memset(&g_stress_state, 0, sizeof(g_stress_state));
    atomic_store_explicit(&g_stress_stop, 0, memory_order_release);
    atomic_store_explicit(&g_stress_failed, 0, memory_order_release);

    pthread_t writer, reader;
    ADCE_TEST_ASSERT(pthread_create(&writer, NULL, stress_writer, NULL) == 0);
    ADCE_TEST_ASSERT(pthread_create(&reader, NULL, stress_reader, NULL) == 0);

    ADCE_TEST_ASSERT(pthread_join(writer, NULL) == 0);
    ADCE_TEST_ASSERT(pthread_join(reader, NULL) == 0);

    ADCE_TEST_ASSERT(atomic_load_explicit(&g_stress_failed, memory_order_acquire) == 0);

    return 0;
}

/* Observation Plane cases. They live in test/t_adce_observe.c, where each is
 * static so the gate's ran-tests guard can find it by source pattern; these
 * are the external forwarders that internal linkage makes necessary to
 * register them in the one runner table below. Each name here must match the
 * suffix of its test_<name> definition over there, because that is the string
 * the guard expects to see in this binary's output. */
int adce_t_obs_sigma_floor(void);
int adce_t_obs_ewma_update(void);
int adce_t_obs_squash(void);
int adce_t_obs_publication_clamp(void);
int adce_t_obs_warmup(void);
int adce_t_obs_writer_claim(void);
int adce_t_obs_cadence(void);
int adce_t_obs_determinism(void);
int adce_t_obs_tap_counter(void);

/* Enforcement Plane cases, same convention: static in test/t_adce_enforce.c so
 * the ran-tests guard sees them, forwarded here to reach this runner table. */
int adce_t_enf_shed_mapping(void);
int adce_t_enf_shed_monotone(void);
int adce_t_enf_read_clamp(void);
int adce_t_enf_stale_fallback(void);
int adce_t_enf_cold_start(void);
int adce_t_enf_bucket_ceiling(void);
int adce_t_enf_determinism(void);
int adce_t_enf_shed_fraction(void);
int adce_t_enf_stale_route_classify(void);
int adce_t_enf_stale_route_equivalence(void);
int adce_t_enf_stale_route_identity(void);

/* Integration harness cases, same convention, in test/t_adce_harness.c. These
 * are the only cases that test a call ORDER rather than a function, so they are
 * the only ones that link the observer thread in src/adce_obs_thread.c. */
int adce_t_harness_tap_before_gate(void);
int adce_t_harness_tap_after_gate(void);
int adce_t_harness_observer_lifecycle(void);
int adce_t_harness_concurrent(void);
int adce_t_harness_stale_posture(void);
int adce_t_harness_stale_split_teeth(void);

/* Per-arrival latency, in test/t_adce_latency.c. Same convention. It REPORTS
 * numbers and asserts only that each fixture drove the outcome it claims, so it
 * cannot fail on a slow or loaded host -- see that file's header for why a
 * timing threshold is deliberately absent. */
int adce_t_latency_per_arrival(void);

/* Closed-loop cases, in test/t_adce_loop.c. Same convention. These are the only
 * cases that run the detector and the actuator against each other over TIME,
 * on a synthetic clock the rig owns, so two runs are bit-identical. They assert
 * no band: docs/closed-loop-harness.md section 5 records the settle band as
 * underived, and loop_step_response_report REPORTS its numbers for that reason
 * -- it is not that document's case 5, which is a ramp and is not implemented. */
int adce_t_loop_synthetic_determinism(void);
int adce_t_loop_draw_invariance(void);
int adce_t_loop_inverted_draw_dependence(void);
int adce_t_loop_settle_metrics_teeth(void);
int adce_t_loop_step_response_report(void);
int adce_t_loop_ramp_fixed_point_report(void);
int adce_t_loop_ramp_below_threshold(void);
int adce_t_loop_ramp_above_threshold(void);
int adce_t_loop_ramp_teeth(void);
int adce_t_loop_bucket_closed_form_report(void);
int adce_t_loop_bucket_conservation(void);
int adce_t_loop_bucket_identity_teeth(void);
int adce_t_loop_bucket_clamp_regime(void);

int main(void) {
    struct {
        const char *name;
        int (*fn)(void);
    } tests[] = {
        {"q16_arithmetic", test_q16_arithmetic},
        {"q16_boundaries", test_q16_boundaries},
        {"token_bucket", test_token_bucket},
        {"rng", test_rng},
        {"platform_entropy", test_platform_entropy},
        {"time_source", test_time_source},
        {"epoch_state_lock_free", test_epoch_state_lock_free},
        {"seqlock_single_threaded", test_seqlock_single_threaded},
        {"seqlock_concurrent", test_seqlock_concurrent},
        {"obs_sigma_floor", adce_t_obs_sigma_floor},
        {"obs_ewma_update", adce_t_obs_ewma_update},
        {"obs_squash", adce_t_obs_squash},
        {"obs_publication_clamp", adce_t_obs_publication_clamp},
        {"obs_warmup", adce_t_obs_warmup},
        {"obs_writer_claim", adce_t_obs_writer_claim},
        {"obs_cadence", adce_t_obs_cadence},
        {"obs_determinism", adce_t_obs_determinism},
        {"obs_tap_counter", adce_t_obs_tap_counter},
        {"enf_shed_mapping", adce_t_enf_shed_mapping},
        {"enf_shed_monotone", adce_t_enf_shed_monotone},
        {"enf_read_clamp", adce_t_enf_read_clamp},
        {"enf_stale_fallback", adce_t_enf_stale_fallback},
        {"enf_cold_start", adce_t_enf_cold_start},
        {"enf_bucket_ceiling", adce_t_enf_bucket_ceiling},
        {"enf_determinism", adce_t_enf_determinism},
        {"enf_shed_fraction", adce_t_enf_shed_fraction},
        {"enf_stale_route_classify", adce_t_enf_stale_route_classify},
        {"enf_stale_route_equivalence", adce_t_enf_stale_route_equivalence},
        {"enf_stale_route_identity", adce_t_enf_stale_route_identity},
        {"harness_tap_before_gate", adce_t_harness_tap_before_gate},
        {"harness_tap_after_gate", adce_t_harness_tap_after_gate},
        {"harness_observer_lifecycle", adce_t_harness_observer_lifecycle},
        {"harness_concurrent", adce_t_harness_concurrent},
        {"harness_stale_posture", adce_t_harness_stale_posture},
        {"harness_stale_split_teeth", adce_t_harness_stale_split_teeth},
        {"latency_per_arrival", adce_t_latency_per_arrival},
        {"loop_synthetic_determinism", adce_t_loop_synthetic_determinism},
        {"loop_draw_invariance", adce_t_loop_draw_invariance},
        {"loop_inverted_draw_dependence", adce_t_loop_inverted_draw_dependence},
        {"loop_settle_metrics_teeth", adce_t_loop_settle_metrics_teeth},
        {"loop_step_response_report", adce_t_loop_step_response_report},
        {"loop_ramp_fixed_point_report", adce_t_loop_ramp_fixed_point_report},
        {"loop_ramp_below_threshold", adce_t_loop_ramp_below_threshold},
        {"loop_ramp_above_threshold", adce_t_loop_ramp_above_threshold},
        {"loop_ramp_teeth", adce_t_loop_ramp_teeth},
        {"loop_bucket_closed_form_report", adce_t_loop_bucket_closed_form_report},
        {"loop_bucket_conservation", adce_t_loop_bucket_conservation},
        {"loop_bucket_identity_teeth", adce_t_loop_bucket_identity_teeth},
        {"loop_bucket_clamp_regime", adce_t_loop_bucket_clamp_regime},
    };

    int failures = 0;
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (tests[i].fn() != 0) {
            fprintf(stderr, "TEST FAILED: %s\n", tests[i].name);
            failures++;
        } else {
            printf("TEST OK: %s\n", tests[i].name);
        }
    }

    if (failures != 0) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }

    printf("All tests passed\n");
    return 0;
}
