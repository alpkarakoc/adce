/* Contended cost of adce_obs_tap. Entry (1) of CLAUDE.md's unverified list.
 *
 * ===========================================================================
 * PRE-REGISTRATION. Everything below was written and committed BEFORE a single
 * measurement was taken; `git log` on this file is the proof of order. Nothing
 * here is adjusted after seeing a number.
 * ===========================================================================
 *
 * WHY THIS EXISTS. test/t_adce_latency.c measures the gate and the clock and
 * spawns no threads; adce_obs_tap appears in it zero times. So the contended
 * cost has ZERO evidence rather than weak evidence, and
 * docs/closed-loop-harness.md section 4 publishes a per-thread bound of
 * 27-59 M arrivals/s derived from gate + clock alone, flagged in that document
 * as an upper bound the tap may not permit.
 *
 * ---------------------------------------------------------------------------
 * PROTOCOL
 * ---------------------------------------------------------------------------
 *
 * SWEEP.  threads T in {1, 2, 4, 8}. The development host is an 8-logical-core
 *         M3 (4P + 4E) and the CI runners are 4 vCPU, so 8 is oversubscribed on
 *         CI and fully subscribed locally. Both are reported; neither is
 *         dropped for being awkward.
 *
 * LOAD.   10,000,000 arrivals PER THREAD, held fixed as T rises. Per-thread
 *         rather than total, because the quantity under test is what ONE thread
 *         achieves while T-1 others contend -- which is exactly the shape of
 *         section 4's claim.
 *
 * RUNS.   10 independent executions per (arm, T). Every figure is reported with
 *         its n beside it.
 *
 * TWO ARMS, and the second is the control that makes this discriminating:
 *
 *   SHARED  -- one adce_obs_counter_t for all T threads. The shipped
 *              configuration: t_adce_harness.c assigns every site the same
 *              counter, and the Observation Plane has exactly one.
 *   PRIVATE -- one adce_obs_counter_t PER THREAD. Same instruction, same
 *              atomic, same alignment, no sharing.
 *
 *   The arms differ in one variable only: whether the line is shared. If the
 *   cost of the atomic RMW itself were the story, both arms would rise
 *   together. If contention is the story, SHARED rises and PRIVATE stays flat.
 *   Without this arm a rise could not be attributed.
 *
 * HELD FIXED across the sweep: arrivals per thread; the binary (one strict -O2
 * build, no sanitizers -- CLAUDE.md records that ASan/TSan dilate execution by
 * ~10x and would measure the sanitizer); the machine; a spin barrier so all T
 * threads are inside the loop together, without which thread 1 could finish
 * before thread T starts and there would be no contention to measure.
 *
 * STATISTIC.  MEDIAN over thread-samples, with MIN and MAX printed beside it.
 *             Not the mean. A descheduled thread produces a large one-sided
 *             outlier, which is a fact about the host's scheduler and not about
 *             the tap; the mean would absorb it and report the host. The min is
 *             the cleanest-schedule estimate and therefore a lower bound on the
 *             cost; the max says how bad a sample got. This mirrors
 *             t_adce_latency.c's reason for reporting per outcome rather than
 *             averaging across outcomes.
 *
 * ---------------------------------------------------------------------------
 * THE DISCRIMINATING PREDICTION, from reading the tap's body, before measuring
 * ---------------------------------------------------------------------------
 *
 * The tap is:
 *
 *     atomic_fetch_add_explicit(&counter->arrivals, 1, memory_order_relaxed);
 *
 * and adce_obs_counter_t is _Alignas(ADCE_CACHELINE) with padding out to a full
 * line. So there is NO FALSE SHARING with neighbouring data -- the padding is
 * doing its job. What the padding cannot prevent is TRUE sharing: one counter,
 * T writers, one line.
 *
 * memory_order_relaxed removes ORDERING, not COHERENCE. A relaxed read-modify-
 * write still needs the line in an exclusive state, so every increment must
 * migrate the line to the incrementing core: `lock xadd` on x86_64, `ldadd`
 * under LSE on arm64, an LL/SC retry loop without it. The increments therefore
 * serialise on one line's ownership no matter how weak the ordering.
 *
 * PREDICTION: hypothesis B.
 *
 *   A (flat)  per-arrival cost approximately independent of T, within ~20%.
 *             The tap would then not be a scaling bottleneck and section 4's
 *             bound would stand for multi-threaded ingress.
 *
 *   B (rises) SHARED per-arrival cost rises monotonically with T, by at least
 *             3x from T=1 to T=8, while PRIVATE stays approximately flat. The
 *             consequence: aggregate throughput saturates instead of scaling,
 *             and per-thread throughput falls.
 *
 * I predict B, and specifically that at T=4 the SHARED per-arrival cost is
 * comparable to or larger than the whole 17-37 ns gate+clock budget section 4
 * sizes against -- which, if it holds, overturns 27-59 M arrivals/s per thread
 * for any ingress with more than one thread.
 *
 * A prediction that merely agreed with whatever came out would be worthless.
 * This one separates two structural readings of the same code, and the PRIVATE
 * arm is what makes the separation attributable rather than inferred.
 *
 * IF THE MEASUREMENT REFUTES B, that is the more valuable output and it will be
 * reported as a refutation, not retrofitted.
 *
 * ---------------------------------------------------------------------------
 * NOT IN scripts/verify.sh, deliberately. This spawns threads and runs for
 * minutes; the per-edit gate is 48 cases in about five seconds and CLAUDE.md
 * records that a latency threshold on a shared runner is a flake generator.
 * It lives in bench/ rather than test/ because verify.sh compiles every test source into
 * one binary with one main(), and its ran-tests guard requires every
 * `static int test_<name>(void)` to appear in the runner table.
 * ===========================================================================
 */

#include "adce_observe.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BENCH_MAX_THREADS 64

typedef struct {
    adce_obs_counter_t *counter;
    uint64_t arrivals;
    uint64_t elapsed_ns;      /* written by the thread, read after join */
    _Atomic uint64_t *ready;  /* barrier: threads-arrived counter */
    _Atomic int *go;          /* barrier: release flag */
    unsigned nthreads;
} bench_thread_t;

static void *bench_main(void *arg) {
    bench_thread_t *t = (bench_thread_t *)arg;
    uint64_t i;
    uint64_t t0;

    /* Spin barrier. All threads must be inside the loop together or there is
     * no contention to measure. Spin rather than condvar: a futex wake would
     * stagger the releases by exactly the scale being measured. */
    atomic_fetch_add_explicit(t->ready, (uint64_t)1, memory_order_acq_rel);
    while (atomic_load_explicit(t->go, memory_order_acquire) == 0) {
        ADCE_CPU_RELAX();
    }

    t0 = adce_now_ns();
    for (i = 0; i < t->arrivals; ++i) {
        adce_obs_tap(t->counter);
    }
    t->elapsed_ns = adce_now_ns() - t0;
    return NULL;
}

/* One execution. Prints one line per thread so the aggregation is done outside
 * this program and every sample is visible rather than pre-summarised. */
static int bench_run(const char *arm, unsigned nthreads, uint64_t arrivals,
                     unsigned run_index) {
    static adce_obs_counter_t shared;
    static adce_obs_counter_t privately[BENCH_MAX_THREADS];
    pthread_t tid[BENCH_MAX_THREADS];
    bench_thread_t ctx[BENCH_MAX_THREADS];
    _Atomic uint64_t ready;
    _Atomic int go;
    unsigned i;
    uint64_t max_elapsed = 0;

    memset(&shared, 0, sizeof(shared));
    memset(privately, 0, sizeof(privately));
    atomic_store_explicit(&ready, (uint64_t)0, memory_order_relaxed);
    atomic_store_explicit(&go, 0, memory_order_relaxed);

    for (i = 0; i < nthreads; ++i) {
        ctx[i].counter = (strcmp(arm, "shared") == 0) ? &shared : &privately[i];
        ctx[i].arrivals = arrivals;
        ctx[i].elapsed_ns = 0;
        ctx[i].ready = &ready;
        ctx[i].go = &go;
        ctx[i].nthreads = nthreads;
        if (pthread_create(&tid[i], NULL, bench_main, &ctx[i]) != 0) {
            fprintf(stderr, "FAIL: pthread_create\n");
            return 1;
        }
    }

    while (atomic_load_explicit(&ready, memory_order_acquire) < nthreads) {
        ADCE_CPU_RELAX();
    }
    atomic_store_explicit(&go, 1, memory_order_release);

    for (i = 0; i < nthreads; ++i) {
        if (pthread_join(tid[i], NULL) != 0) {
            fprintf(stderr, "FAIL: pthread_join\n");
            return 1;
        }
    }

    for (i = 0; i < nthreads; ++i) {
        if (ctx[i].elapsed_ns > max_elapsed) {
            max_elapsed = ctx[i].elapsed_ns;
        }
        printf("SAMPLE arm=%s threads=%u run=%u thread=%u arrivals=%llu"
               " elapsed_ns=%llu ns_per_arrival=%.4f\n",
               arm, nthreads, run_index, i, (unsigned long long)arrivals,
               (unsigned long long)ctx[i].elapsed_ns,
               (double)ctx[i].elapsed_ns / (double)arrivals);
    }
    printf("RUN arm=%s threads=%u run=%u wall_ns=%llu"
           " aggregate_per_s=%.0f\n",
           arm, nthreads, run_index, (unsigned long long)max_elapsed,
           max_elapsed ? (double)arrivals * nthreads * 1e9 / (double)max_elapsed
                       : 0.0);

    /* The shared arm's counter must hold exactly what was tapped. A lost
     * increment would mean the thing being timed is not the thing that runs. */
    if (strcmp(arm, "shared") == 0) {
        uint64_t got = atomic_load_explicit(&shared.arrivals,
                                            memory_order_relaxed);
        if (got != arrivals * nthreads) {
            fprintf(stderr, "FAIL: shared counter %llu != %llu\n",
                    (unsigned long long)got,
                    (unsigned long long)(arrivals * nthreads));
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    const unsigned sweep[] = {1u, 2u, 4u, 8u};
    uint64_t arrivals = (argc > 1) ? strtoull(argv[1], NULL, 10) : 10000000ULL;
    unsigned runs = (argc > 2) ? (unsigned)strtoul(argv[2], NULL, 10) : 10u;
    unsigned r;
    size_t s;

    printf("BENCH tap contention | arrivals_per_thread=%llu runs=%u"
           " cacheline=%d sizeof(counter)=%zu\n",
           (unsigned long long)arrivals, runs, (int)ADCE_CACHELINE,
           sizeof(adce_obs_counter_t));

    for (s = 0; s < sizeof(sweep) / sizeof(sweep[0]); ++s) {
        for (r = 0; r < runs; ++r) {
            if (bench_run("shared", sweep[s], arrivals, r) != 0) {
                return 1;
            }
            if (bench_run("private", sweep[s], arrivals, r) != 0) {
                return 1;
            }
        }
        fflush(stdout);
    }
    printf("BENCH done\n");
    return 0;
}
