#ifndef ADCE_PLATFORM_H
#define ADCE_PLATFORM_H

#if defined(__cplusplus)
extern "C" {
#endif

#if !defined(__linux__)
#error "ADCE requires Linux (CLOCK_MONOTONIC_RAW and getrandom() are Linux-specific)"
#endif

#if !defined(__x86_64__)
#error "ADCE requires the x86_64 architecture (64-Byte L1D cache line, CMPXCHG8B lock-free U64 guarantee)"
#endif

#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 201112L
#error "ADCE requires a C11 (or later) compiler"
#endif

#if !defined(__SIZEOF_INT128__)
#error "ADCE requires compiler support for 128-bit integer extensions (__int128 / unsigned __int128)"
#endif

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/random.h>

/* ===========================================================================
 * Hardware profile
 * ===========================================================================
 */

#define ADCE_CACHE_LINE_SIZE ((size_t)64U)
#define ADCE_ALIGNED _Alignas(ADCE_CACHE_LINE_SIZE)

_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2,
               "64-bit atomics must be always-lock-free on this target");
_Static_assert(sizeof(_Atomic uint64_t) == sizeof(uint64_t),
               "atomic uint64_t must not carry hidden lock-object overhead");

static inline void adce_cpu_relax(void) {
    __builtin_ia32_pause();
}

/* ===========================================================================
 * Time source: CLOCK_MONOTONIC_RAW only. CLOCK_REALTIME and CLOCK_MONOTONIC
 * are prohibited project-wide to eliminate NTP slew as an attack surface on
 * window/timeout arithmetic.
 * ===========================================================================
 */

static inline uint64_t adce_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

#if !defined(ADCE_ADVICE_TIMEOUT_NS)
#define ADCE_ADVICE_TIMEOUT_NS (50ULL * 1000ULL * 1000ULL) /* 50ms */
#endif

/* ===========================================================================
 * Fixed-point Q16.16 (Ingest and Enforcement planes only; the Observation
 * Plane is where floating point is permitted).
 * ===========================================================================
 */

typedef int64_t adce_q16_t;

#define ADCE_Q16_FRAC_BITS 16
#define ADCE_Q16_ONE ((adce_q16_t)1 << ADCE_Q16_FRAC_BITS)
#define ADCE_Q16_MAX ((adce_q16_t)INT64_MAX)
#define ADCE_Q16_MIN ((adce_q16_t)INT64_MIN)

static inline adce_q16_t adce_q16_from_int(int32_t v) {
    return (adce_q16_t)v << ADCE_Q16_FRAC_BITS;
}

static inline int32_t adce_q16_to_int(adce_q16_t v) {
    return (int32_t)(v >> ADCE_Q16_FRAC_BITS);
}

static inline adce_q16_t adce_q16_add(adce_q16_t a, adce_q16_t b) {
    return a + b;
}

static inline adce_q16_t adce_q16_sub(adce_q16_t a, adce_q16_t b) {
    return a - b;
}

static inline adce_q16_t adce_q16_mul(adce_q16_t a, adce_q16_t b) {
    __int128 wide = (__int128)a * (__int128)b;
    return (adce_q16_t)(wide >> ADCE_Q16_FRAC_BITS);
}

static inline adce_q16_t adce_q16_div(adce_q16_t a, adce_q16_t b) {
    __int128 wide = (__int128)a << ADCE_Q16_FRAC_BITS;
    return (adce_q16_t)(wide / (__int128)b);
}

static inline adce_q16_t adce_q16_min(adce_q16_t a, adce_q16_t b) {
    return a < b ? a : b;
}

static inline adce_q16_t adce_q16_max(adce_q16_t a, adce_q16_t b) {
    return a > b ? a : b;
}

/* ===========================================================================
 * Overflow-free token bucket arithmetic. Refill accumulation is computed in
 * unsigned __int128 so that (rate * elapsed) can never wrap a 64-bit lane
 * before it is clamped back down to the bucket's Q16.16 capacity.
 * ===========================================================================
 */

typedef unsigned __int128 adce_u128_t;

static inline uint64_t adce_token_refill(uint64_t current_tokens_q16,
                                          uint64_t rate_q16_per_ns,
                                          uint64_t elapsed_ns,
                                          uint64_t capacity_q16) {
    adce_u128_t added = (adce_u128_t)rate_q16_per_ns * (adce_u128_t)elapsed_ns;
    adce_u128_t total = (adce_u128_t)current_tokens_q16 + added;
    adce_u128_t cap = (adce_u128_t)capacity_q16;
    return (uint64_t)(total > cap ? cap : total);
}

static inline int adce_token_try_take(uint64_t *tokens_q16, uint64_t cost_q16) {
    if (*tokens_q16 < cost_q16) {
        return 0;
    }
    *tokens_q16 -= cost_q16;
    return 1;
}

/* ===========================================================================
 * Per-thread xorshift128+ PRNG. Zero-allocation, lock-free, seeded once per
 * thread from getrandom(). libc rand()/random() are banned: their internal
 * state is protected by a lock, which is inadmissible on the hot path.
 * ===========================================================================
 */

typedef struct {
    uint64_t state[2];
    int initialized;
} adce_rng_state_t;

static _Thread_local adce_rng_state_t adce_rng_tls = {{0, 0}, 0};

static inline void adce_rng_seed(adce_rng_state_t *rng) {
    uint8_t buf[16];
    size_t got = 0;

    while (got < sizeof(buf)) {
        ssize_t r = getrandom(buf + got, sizeof(buf) - got, 0);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            abort();
        }
        got += (size_t)r;
    }

    memcpy(rng->state, buf, sizeof(rng->state));

    if (rng->state[0] == 0 && rng->state[1] == 0) {
        rng->state[0] = 0x9E3779B97F4A7C15ULL;
        rng->state[1] = 0xBF58476D1CE4E5B9ULL;
    }

    rng->initialized = 1;
}

static inline uint64_t adce_rng_next(void) {
    adce_rng_state_t *rng = &adce_rng_tls;

    if (!rng->initialized) {
        adce_rng_seed(rng);
    }

    uint64_t s1 = rng->state[0];
    const uint64_t s0 = rng->state[1];
    const uint64_t result = s0 + s1;

    rng->state[0] = s0;
    s1 ^= s1 << 23;
    s1 ^= s1 >> 17;
    s1 ^= s0;
    s1 ^= s0 >> 26;
    rng->state[1] = s1;

    return result;
}

/* Uniform double in [0, 1) built from the top 53 mantissa bits. Confined to
 * the Observation Plane by convention: the Ingest/Enforcement planes must
 * drive stochastic drop decisions from adce_rng_next() and integer
 * comparisons only. */
static inline double adce_rng_next_unit(void) {
    return (double)(adce_rng_next() >> 11) * (1.0 / 9007199254740992.0);
}

/* ===========================================================================
 * Wait-free, single-cache-line Seqlock transport. No mutexes, no spinlocks,
 * no allocation. A single writer publishes; any number of readers retry
 * until they observe a stable (even, matching) sequence.
 * ===========================================================================
 */

typedef _Atomic uint64_t adce_seq_t;

static inline void adce_seqlock_write_begin(adce_seq_t *seq) {
    uint64_t s = atomic_load_explicit(seq, memory_order_relaxed);
    atomic_store_explicit(seq, s + 1, memory_order_release);
    atomic_thread_fence(memory_order_acquire);
}

static inline void adce_seqlock_write_end(adce_seq_t *seq) {
    uint64_t s = atomic_load_explicit(seq, memory_order_relaxed);
    atomic_store_explicit(seq, s + 1, memory_order_release);
}

static inline uint64_t adce_seqlock_read_begin(const adce_seq_t *seq) {
    uint64_t s;
    do {
        s = atomic_load_explicit(seq, memory_order_acquire);
        if (s & 1U) {
            adce_cpu_relax();
        }
    } while (s & 1U);
    return s;
}

static inline int adce_seqlock_read_retry(const adce_seq_t *seq, uint64_t start) {
    atomic_thread_fence(memory_order_acquire);
    uint64_t s = atomic_load_explicit(seq, memory_order_acquire);
    return s != start;
}

/* ===========================================================================
 * Shared epoch state: the canonical cross-plane publication object. The
 * Observation Plane writes it; the Enforcement Plane reads it. It occupies
 * exactly one cache line so publication is always torn-free between
 * sequence flips and never straddles a false-sharing boundary.
 * ===========================================================================
 */

/* _Alignas cannot legally appear on a typedef declaration itself (C11
 * 6.7.5p2); placing it on the first member instead forces the struct's own
 * alignment up to a full cache line. */
typedef struct {
    _Alignas(ADCE_CACHE_LINE_SIZE) adce_seq_t sequence;
    adce_q16_t pressure;
    uint64_t epoch_id;
    uint64_t observed_at_ns;
    uint8_t _reserved[ADCE_CACHE_LINE_SIZE - sizeof(adce_seq_t) -
                       sizeof(adce_q16_t) - sizeof(uint64_t) - sizeof(uint64_t)];
} adce_epoch_state_t;

_Static_assert(sizeof(adce_epoch_state_t) == ADCE_CACHE_LINE_SIZE,
               "adce_epoch_state_t must occupy exactly one cache line");
_Static_assert(_Alignof(adce_epoch_state_t) == ADCE_CACHE_LINE_SIZE,
               "adce_epoch_state_t must be cache-line aligned");

static inline void adce_epoch_publish(adce_epoch_state_t *state,
                                       adce_q16_t pressure,
                                       uint64_t epoch_id,
                                       uint64_t observed_at_ns) {
    adce_seqlock_write_begin(&state->sequence);
    state->pressure = pressure;
    state->epoch_id = epoch_id;
    state->observed_at_ns = observed_at_ns;
    adce_seqlock_write_end(&state->sequence);
}

static inline int adce_epoch_read(const adce_epoch_state_t *state,
                                   adce_q16_t *pressure,
                                   uint64_t *epoch_id,
                                   uint64_t *observed_at_ns) {
    uint64_t s0 = adce_seqlock_read_begin(&state->sequence);

    adce_q16_t p = state->pressure;
    uint64_t e = state->epoch_id;
    uint64_t t = state->observed_at_ns;

    if (adce_seqlock_read_retry(&state->sequence, s0)) {
        return 0;
    }

    *pressure = p;
    *epoch_id = e;
    *observed_at_ns = t;
    return 1;
}

/* Fail-closed watchdog: true when the last published epoch is stale enough
 * that the Enforcement Plane must stop trusting the Observation Plane's
 * advice and fall back to its most conservative posture. */
static inline int adce_epoch_is_stale(uint64_t observed_at_ns, uint64_t now_ns) {
    return (now_ns - observed_at_ns) > ADCE_ADVICE_TIMEOUT_NS;
}

#if defined(__cplusplus)
}
#endif

#endif /* ADCE_PLATFORM_H */
