#ifndef ADCE_PLATFORM_H
#define ADCE_PLATFORM_H

#if defined(__cplusplus)
extern "C" {
#endif

#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 201112L
#error "ADCE requires a C11 (or later) compiler"
#endif

#if !defined(__SIZEOF_INT128__)
#error "ADCE requires compiler support for 128-bit integer extensions (__int128 / unsigned __int128)"
#endif

#include <errno.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ===========================================================================
 * Platform abstraction.
 *
 * This is the ONLY architecture- or OS-conditional block in the codebase.
 * Everything below it is written against exactly three macros:
 *
 *   ADCE_CACHELINE           false-sharing granule in bytes
 *   ADCE_CPU_RELAX()         spin-wait hint for the seqlock retry loop
 *   ADCE_GET_ENTROPY(b, n)   fill n bytes of b; 0 on success, -1 on failure
 *
 * Shipping target is Linux x86_64. arm64 is supported because its weak memory
 * model is a stricter test of the seqlock than x86_64's TSO: code that passes
 * on arm64 passes on x86_64, and the reverse does not hold. An #error here
 * means the target genuinely lacks one of these primitives, not merely that
 * it is not the shipping target.
 * ===========================================================================
 */

#if defined(__x86_64__) || defined(__amd64__)

#define ADCE_CACHELINE ((size_t)64U)
#define ADCE_CPU_RELAX() __builtin_ia32_pause()

#elif defined(__aarch64__) || defined(__arm64__)

/* 128, not 64: Apple M-series and Neoverse cores fetch and stripe on a
 * 128-byte granule, so a 64-byte-padded object still false-shares. */
#define ADCE_CACHELINE ((size_t)128U)
#define ADCE_CPU_RELAX() __asm__ __volatile__("yield")

#else
#error "ADCE supports x86_64 and arm64 only: no cache-line width or CPU-relax primitive is defined for this architecture"
#endif

#if defined(__linux__)

#include <sys/random.h>

/* getrandom() returns short on signal delivery. A short fill is a silently
 * weakened seed, so loop to completion and report failure rather than
 * handing back a byte count the caller has to remember to check. */
static inline int adce_platform_get_entropy(void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;

    while (got < len) {
        ssize_t r = getrandom(p + got, len - got, 0);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (r == 0) {
            return -1;
        }
        got += (size_t)r;
    }

    return 0;
}

#elif defined(__APPLE__)

#include <sys/random.h>

/* getentropy() is all-or-nothing per call but refuses any request over 256
 * bytes, so the chunking loop is mandatory, not defensive. */
static inline int adce_platform_get_entropy(void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;

    while (got < len) {
        size_t chunk = len - got;
        if (chunk > 256U) {
            chunk = 256U;
        }
        if (getentropy(p + got, chunk) != 0) {
            return -1;
        }
        got += chunk;
    }

    return 0;
}

#else
#error "ADCE requires a kernel entropy syscall: getrandom() (Linux) or getentropy() (macOS)"
#endif

#define ADCE_GET_ENTROPY(buf, len) adce_platform_get_entropy((buf), (len))

/* ===========================================================================
 * Hardware profile
 * ===========================================================================
 */

_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2,
               "64-bit atomics must be always-lock-free on this target");
_Static_assert(sizeof(_Atomic uint64_t) == sizeof(uint64_t),
               "atomic uint64_t must not carry hidden lock-object overhead");

/* The publication object's payload is _Atomic, so its layout is only
 * predictable while _Atomic is a pure qualifier here. A compiler that widens
 * or over-aligns these types would silently repack adce_epoch_state_t; these
 * four assertions exist so that fails the build instead. Do not "fix" a
 * failure here by re-tuning the padding -- the target has changed, and the
 * single-cache-line publication design has to be re-decided. */
_Static_assert(_Alignof(_Atomic uint64_t) == _Alignof(uint64_t),
               "atomic uint64_t must not change alignment");
_Static_assert(sizeof(_Atomic int64_t) == sizeof(int64_t),
               "atomic int64_t must not carry hidden lock-object overhead");
_Static_assert(_Alignof(_Atomic int64_t) == _Alignof(int64_t),
               "atomic int64_t must not change alignment");

/* ===========================================================================
 * Time source: CLOCK_MONOTONIC_RAW only. CLOCK_REALTIME and CLOCK_MONOTONIC
 * are prohibited project-wide to eliminate NTP slew as an attack surface on
 * window/timeout arithmetic. Available on Linux and on macOS 10.12+, so no
 * fallback clock is defined.
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

/* No range rejection is possible on the way in: |INT32_MIN| << 16 is 2^47,
 * far inside the int64_t lane, so a runtime check could never fire. The
 * invariant is pinned here instead of paid for on every call. */
_Static_assert(32 + ADCE_Q16_FRAC_BITS < 64,
               "int32_t must fit the Q16.16 lane after the fractional shift");

static inline adce_q16_t adce_q16_from_int(int32_t v) {
    /* Shift through unsigned width. Left-shifting a negative signed value is
     * undefined (C11 6.5.7p4) regardless of what the target emits; the
     * unsigned shift is defined mod 2^64 and converting back reproduces
     * v * 2^16 exactly on two's-complement. */
    return (adce_q16_t)((uint64_t)(int64_t)v << ADCE_Q16_FRAC_BITS);
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
    /* A zero divisor has no representable quotient. Saturate toward the
     * numerator's sign -- the mathematical limit -- so a collapsed divisor
     * reads as maximal rather than zero pressure downstream. */
    if (b == 0) {
        return a < 0 ? ADCE_Q16_MIN : ADCE_Q16_MAX;
    }

    /* Same unsigned-width detour as adce_q16_from_int, taken at __int128
     * width so a full-range Q16.16 numerator keeps its precision. The cast
     * back is implementation-defined, NOT undefined (C11 6.3.1.3p3): GCC and
     * Clang both define an out-of-range unsigned->signed conversion as
     * modular two's-complement wrap, which is the bit pattern relied on here.
     */
    __int128 wide =
        (__int128)((unsigned __int128)(__int128)a << ADCE_Q16_FRAC_BITS);
    __int128 q = wide / (__int128)b;

    /* ADCE_Q16_MIN / -1 is +2^79. Widening before the divide keeps the
     * division itself safe, but the quotient still outruns the 64-bit lane,
     * so clamp it rather than narrow it silently. */
    if (q > (__int128)ADCE_Q16_MAX) {
        return ADCE_Q16_MAX;
    }
    if (q < (__int128)ADCE_Q16_MIN) {
        return ADCE_Q16_MIN;
    }
    return (adce_q16_t)q;
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
 * thread from the kernel entropy source behind ADCE_GET_ENTROPY. libc
 * rand()/random() are banned: their internal state is protected by a lock,
 * which is inadmissible on the hot path.
 * ===========================================================================
 */

typedef struct {
    uint64_t state[2];
    int initialized;
} adce_rng_state_t;

static _Thread_local adce_rng_state_t adce_rng_tls = {{0, 0}, 0};

static inline void adce_rng_seed(adce_rng_state_t *rng) {
    uint8_t buf[16];

    /* Fail-closed. A PRNG seeded from a failed or partial entropy draw is
     * predictable, and every stochastic containment decision downstream
     * inherits that; there is deliberately no degraded seeding path. */
    if (ADCE_GET_ENTROPY(buf, sizeof(buf)) != 0) {
        abort();
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
            ADCE_CPU_RELAX();
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
    _Alignas(ADCE_CACHELINE) adce_seq_t sequence;
    _Atomic adce_q16_t pressure;
    _Atomic uint64_t epoch_id;
    _Atomic uint64_t observed_at_ns;
    uint8_t _reserved[ADCE_CACHELINE - sizeof(adce_seq_t) -
                       sizeof(_Atomic adce_q16_t) - sizeof(_Atomic uint64_t) -
                       sizeof(_Atomic uint64_t)];
} adce_epoch_state_t;

_Static_assert(sizeof(adce_epoch_state_t) == ADCE_CACHELINE,
               "adce_epoch_state_t must occupy exactly one cache line");
_Static_assert(_Alignof(adce_epoch_state_t) == ADCE_CACHELINE,
               "adce_epoch_state_t must be cache-line aligned");

static inline void adce_epoch_publish(adce_epoch_state_t *state,
                                       adce_q16_t pressure,
                                       uint64_t epoch_id,
                                       uint64_t observed_at_ns) {
    adce_seqlock_write_begin(&state->sequence);
    /* Relaxed is sufficient and is the point: the payload carries no ordering
     * of its own, and the two release increments bracketing it are what make
     * these stores visible. Anything stronger buys a barrier per field for
     * nothing. */
    atomic_store_explicit(&state->pressure, pressure, memory_order_relaxed);
    atomic_store_explicit(&state->epoch_id, epoch_id, memory_order_relaxed);
    atomic_store_explicit(&state->observed_at_ns, observed_at_ns,
                          memory_order_relaxed);
    adce_seqlock_write_end(&state->sequence);
}

static inline int adce_epoch_read(const adce_epoch_state_t *state,
                                   adce_q16_t *pressure,
                                   uint64_t *epoch_id,
                                   uint64_t *observed_at_ns) {
    uint64_t s0 = adce_seqlock_read_begin(&state->sequence);

    /* Relaxed atomic loads, not plain loads. Detecting a torn read after the
     * fact does not make the read legal: concurrent non-atomic access is a
     * data race, and a data race is UB that licenses the compiler to split,
     * refetch, or hoist these loads out of the retry entirely. The acquire
     * fence that orders them against the second sequence read is inside
     * adce_seqlock_read_retry. */
    adce_q16_t p = atomic_load_explicit(&state->pressure, memory_order_relaxed);
    uint64_t e = atomic_load_explicit(&state->epoch_id, memory_order_relaxed);
    uint64_t t =
        atomic_load_explicit(&state->observed_at_ns, memory_order_relaxed);

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
