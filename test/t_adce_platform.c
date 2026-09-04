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

int main(void) {
    struct {
        const char *name;
        int (*fn)(void);
    } tests[] = {
        {"q16_arithmetic", test_q16_arithmetic},
        {"q16_boundaries", test_q16_boundaries},
        {"token_bucket", test_token_bucket},
        {"rng", test_rng},
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
