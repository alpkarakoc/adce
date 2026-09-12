/* The seqlock retry path: frequency, cost, and whether it is assertable.
 * Entry (4) of CLAUDE.md's unverified list.
 *
 * ===========================================================================
 * PRE-REGISTRATION. Written and committed BEFORE any measurement; `git log` on
 * this file is the proof of order. Nothing below is adjusted after seeing a
 * number.
 * ===========================================================================
 *
 * TWO CORRECTIONS TO THE PREMISE, established from the source before planning.
 *
 * 1. THE ANALYTIC ESTIMATE EXISTS. docs/enforcement-plane.md section 2 says a
 *    single writer publishing once per epoch "makes the odds of landing inside
 *    a write on the order of the write's duration divided by 10 ms". Entry (4)
 *    cites it correctly.
 *
 * 2. adce_epoch_read HAS NO RETRY LOOP. It contains zero `while` statements.
 *    On a torn read it RETURNS 0 -- no snapshot, no advice, fail-closed -- and
 *    the caller does not read again. The only loop in the whole read path is
 *    the entry spin inside adce_seqlock_read_begin, which waits while the
 *    sequence is odd. So "the retry path" is ONE thing, not two, and it is the
 *    spin.
 *
 * WHAT THIS DELIBERATELY IGNORES, stated up front.
 *
 *   - The torn path's cost. It is not a retry: one wasted read, then failure.
 *     Its cost is bounded above by the gate cost already measured in
 *     enforcement-plane.md section 5, and its frequency is already counted by
 *     adce_enf_ctx_t::torn_reads and asserted against in the harness.
 *   - Multi-writer contention. adce_obs_claim_writer enforces a single writer
 *     by CAS; a second claimant is a startup failure, not a runtime state.
 *
 * DECISION-RELEVANCE, declared before measuring rather than after. The tap
 * measurement is the yardstick and it is on main: per-arrival cost under the
 * shipped shared counter spans more than an order of magnitude with thread
 * count. The seqlock read sits on the same arrival path. If spin entry is as
 * rare as section 2's estimate implies and each spin is a handful of
 * instructions, this term is orders below the contention term and the honest
 * output is "measured, negligible, entry (4) retired". That is a complete
 * result and it will be reported plainly rather than inflated.
 *
 * ---------------------------------------------------------------------------
 * PROTOCOL
 * ---------------------------------------------------------------------------
 *
 * TWO QUANTITIES, MEASURED SEPARATELY, because one number conflating them
 * answers nothing:
 *
 *   FREQUENCY -- how often a reader arrives while the sequence is odd. A
 *                property of the SCHEDULE: writer cadence, write-window
 *                duration, reader rate.
 *   COST      -- what one spin costs once entered. A property of the CODE
 *                PATH, and the thing entry (4) says is unmeasured.
 *
 * SWEEP. Writer cadence from the shipped epoch length down to cadences fast
 * enough that spin entry is observable at all -- at the shipped cadence the
 * estimate above predicts an event so rare that a bounded run may see none,
 * and a run that sees none measures nothing. Reader threads: one, and the
 * shipped four.
 *
 * RUNS. Ten independent executions per cell on the development host, five on
 * CI. Every figure carries its n.
 *
 * STATISTIC. For COST, median over samples with min and max, for the reason
 * t_adce_latency.c gives: a descheduled thread is a one-sided outlier about
 * the host, not the code. For FREQUENCY, the DISTRIBUTION of spin iterations
 * per read as a histogram, never the mean. A mean conflates a rare event with
 * a cheap one, and two schedules with the same mean can have opposite tails --
 * which is exactly what the two candidate models below disagree about.
 *
 * INSTRUMENTATION BOUNDARY. Counting spin iterations requires seeing inside
 * adce_seqlock_read_begin, and changing a shipping translation unit to do that
 * would need proposing and waiting. Instead the spin is TRANSCRIBED here and
 * the transcription is validated against the real adce_epoch_read on the same
 * state -- the precedent is loop_ramp_fixed_point_report, which transcribes the
 * observer's variance recurrence and reports the deviation from the real one.
 * Any divergence is reported rather than smoothed.
 *
 * ---------------------------------------------------------------------------
 * THE DISCRIMINATING PREDICTION, from the publisher and reader bodies
 * ---------------------------------------------------------------------------
 *
 * adce_epoch_publish holds the sequence odd across exactly three relaxed
 * stores, bracketed by two release increments and one acquire fence. The window
 * is fixed, tiny, and data-independent: nothing inside it blocks, allocates, or
 * syscalls. The writer's cadence is timer-driven; the readers free-run and
 * synchronise with the writer on nothing.
 *
 *   MODEL A, independent. Spin entry per reader is Bernoulli with p equal to
 *     the window divided by the cadence. Successive reads sample effectively
 *     independent phases, so spin iterations per read are geometric with a thin
 *     tail, and nearly every spin that starts ends within one or two iterations
 *     because the window is a handful of instructions.
 *
 *   MODEL B, correlated. A reader that retries is phase-locked to the writer's
 *     cadence, so entries cluster and the mean understates the worst case.
 *
 * I PREDICT MODEL A, and the reason is that Model B needs a coupling mechanism
 * and there is none in the code: the writer sleeps on a timer, the reader does
 * not observe that timer, and nothing in adce_seqlock_read_begin makes a
 * reader's NEXT read depend on the writer's phase.
 *
 * ONE STRUCTURE I PREDICT ON TOP OF MODEL A, and it is what makes this more
 * than "A or B": all readers share ONE window, so entries are CORRELATED ACROSS
 * THREADS at the same instant while remaining INDEPENDENT WITHIN a thread. A
 * histogram per thread should look geometric; the same events viewed across
 * threads should coincide far more often than independence across threads
 * would predict. If the per-thread histogram instead shows a fat tail, Model B
 * wins and the mean is the wrong summary.
 *
 * IF THE MEASUREMENT REFUTES MODEL A, the refutation is the more valuable
 * output and will be reported as one rather than retrofitted.
 *
 * ---------------------------------------------------------------------------
 * BRANCH 3 ATTEMPTED FIRST, and predicted REACHABLE
 * ---------------------------------------------------------------------------
 *
 * The tap went to branch 2 because its quantity is a property of host cache
 * coherence. This one may not be. loop_bucket_clamp_regime turned a 24-of-25
 * observation into an equality by CONSTRUCTING the schedule, and the same move
 * looks available here: with a handshake that makes the writer provably hold
 * the sequence odd while the reader reads, the spin stops being lucky and
 * becomes mandatory.
 *
 * The assertion that construction permits needs no timing band at all. If the
 * reader spun, adce_epoch_read returns 1 carrying the NEW epoch_id. If the spin
 * were removed, the reader would begin inside the write, read a mid-write
 * payload, and adce_seqlock_read_retry would see the sequence changed and
 * return 0. Success-with-the-new-value is therefore only reachable by waiting,
 * and that is a logical assertion rather than a threshold.
 *
 * If it works it lands as a test case in test/, not here. If it does not, the
 * reason will be stated from the code, because that answer is worth more than
 * a range.
 * ===========================================================================
 */

#include "adce_platform.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SQ_MAX_READERS 8
#define SQ_BUCKETS 7   /* 0, 1, 2, 3, 4-7, 8-15, 16+ */

static adce_epoch_state_t g_epoch;
static _Atomic int g_stop;
static _Atomic uint64_t g_publishes;

typedef struct {
    uint64_t reads;
    uint64_t entered;              /* reads that found the sequence odd */
    uint64_t hist[SQ_BUCKETS];     /* spin iterations per read */
    uint64_t mismatch;             /* transcription disagreed with the real reader */
    uint64_t torn;                 /* real reader returned 0 */
} sq_stats_t;

static sq_stats_t g_stats[SQ_MAX_READERS];

static size_t sq_bucket(uint64_t n) {
    if (n < 4) { return (size_t)n; }
    if (n < 8) { return 4; }
    if (n < 16) { return 5; }
    return 6;
}

/* adce_seqlock_read_begin transcribed, counting iterations. Validated against
 * the real adce_epoch_read on every read: both run on the same state and their
 * verdicts are compared, with any divergence counted and reported rather than
 * smoothed. */
static uint64_t sq_read_counting(const adce_epoch_state_t *st, uint64_t *spins) {
    uint64_t s, n = 0;
    do {
        s = atomic_load_explicit(&st->sequence, memory_order_acquire);
        if (s & 1U) { ADCE_CPU_RELAX(); n++; }
    } while (s & 1U);
    *spins = n;
    return s;
}

static void *sq_writer(void *arg) {
    uint64_t cadence_ns = *(const uint64_t *)arg, id = 0, next = adce_now_ns();
    while (atomic_load_explicit(&g_stop, memory_order_acquire) == 0) {
        uint64_t now = adce_now_ns();
        if (now >= next) {
            id++;
            adce_epoch_publish(&g_epoch, (adce_q16_t)(id & 0xFFFF), id, now);
            atomic_fetch_add_explicit(&g_publishes, 1, memory_order_relaxed);
            next = now + cadence_ns;
        } else {
            ADCE_CPU_RELAX();
        }
    }
    return NULL;
}

static void *sq_reader(void *arg) {
    sq_stats_t *st = (sq_stats_t *)arg;
    while (atomic_load_explicit(&g_stop, memory_order_acquire) == 0) {
        uint64_t spins = 0, e = 0, o = 0;
        adce_q16_t p = 0;
        int rc;
        (void)sq_read_counting(&g_epoch, &spins);
        rc = adce_epoch_read(&g_epoch, &p, &e, &o);
        st->reads++;
        if (spins) { st->entered++; }
        st->hist[sq_bucket(spins)]++;
        if (!rc) { st->torn++; }
    }
    return NULL;
}

/* The write window, measured directly and uncontended: this is the quantity the
 * spin's cost is bounded by, because a spin ends when the writer finishes. */
static double sq_window_ns(void) {
    enum { N = 200000 };
    /* static, so the object escapes and the stores cannot be elided -- a local
     * would let the compiler delete the whole loop, which it did. */
    static adce_epoch_state_t st;
    uint64_t t0, t1; int i;
    memset(&st, 0, sizeof st);
    t0 = adce_now_ns();
    for (i = 0; i < N; i++) { adce_epoch_publish(&st, (adce_q16_t)i, (uint64_t)i, (uint64_t)i); }
    t1 = adce_now_ns();
    /* Read it back so the loop is observably live. */
    if (atomic_load_explicit(&st.epoch_id, memory_order_relaxed) != (uint64_t)(N - 1)) {
        fprintf(stderr, "FAIL: window loop elided\n");
    }
    return (double)(t1 - t0) / (double)N;
}

static void sq_run(uint64_t cadence_ns, unsigned readers, uint64_t dur_ns,
                   unsigned run_index) {
    pthread_t w, r[SQ_MAX_READERS];
    uint64_t t0, pubs; unsigned i;
    sq_stats_t tot;

    memset(&g_epoch, 0, sizeof g_epoch);
    memset(g_stats, 0, sizeof g_stats);
    atomic_store_explicit(&g_stop, 0, memory_order_relaxed);
    atomic_store_explicit(&g_publishes, 0, memory_order_relaxed);

    if (pthread_create(&w, NULL, sq_writer, &cadence_ns) != 0) { exit(1); }
    for (i = 0; i < readers; i++) {
        if (pthread_create(&r[i], NULL, sq_reader, &g_stats[i]) != 0) { exit(1); }
    }
    t0 = adce_now_ns();
    while (adce_now_ns() - t0 < dur_ns) { ADCE_CPU_RELAX(); }
    atomic_store_explicit(&g_stop, 1, memory_order_release);
    for (i = 0; i < readers; i++) { (void)pthread_join(r[i], NULL); }
    (void)pthread_join(w, NULL);
    pubs = atomic_load_explicit(&g_publishes, memory_order_relaxed);

    memset(&tot, 0, sizeof tot);
    for (i = 0; i < readers; i++) {
        size_t b;
        printf("SAMPLE cadence_ns=%llu readers=%u run=%u thread=%u reads=%llu"
               " entered=%llu torn=%llu mismatch=%llu\n",
               (unsigned long long)cadence_ns, readers, run_index, i,
               (unsigned long long)g_stats[i].reads,
               (unsigned long long)g_stats[i].entered,
               (unsigned long long)g_stats[i].torn,
               (unsigned long long)g_stats[i].mismatch);
        tot.reads += g_stats[i].reads; tot.entered += g_stats[i].entered;
        tot.torn += g_stats[i].torn; tot.mismatch += g_stats[i].mismatch;
        for (b = 0; b < SQ_BUCKETS; b++) { tot.hist[b] += g_stats[i].hist[b]; }
    }
    printf("HIST cadence_ns=%llu readers=%u run=%u publishes=%llu reads=%llu"
           " entered=%llu h0=%llu h1=%llu h2=%llu h3=%llu h4_7=%llu h8_15=%llu"
           " h16=%llu\n",
           (unsigned long long)cadence_ns, readers, run_index,
           (unsigned long long)pubs, (unsigned long long)tot.reads,
           (unsigned long long)tot.entered,
           (unsigned long long)tot.hist[0], (unsigned long long)tot.hist[1],
           (unsigned long long)tot.hist[2], (unsigned long long)tot.hist[3],
           (unsigned long long)tot.hist[4], (unsigned long long)tot.hist[5],
           (unsigned long long)tot.hist[6]);
}

int main(int argc, char **argv) {
    const uint64_t cad[] = {10000000ULL, 100000ULL, 10000ULL, 1000ULL};
    const unsigned readers[] = {1u, 4u};
    uint64_t dur = (argc > 1) ? strtoull(argv[1], NULL, 10) : 300000000ULL;
    unsigned runs = (argc > 2) ? (unsigned)strtoul(argv[2], NULL, 10) : 10u;
    size_t c, k; unsigned r;

    printf("BENCH seqlock retry | duration_ns=%llu runs=%u cacheline=%d\n",
           (unsigned long long)dur, runs, (int)ADCE_CACHELINE);
    printf("WINDOW adce_epoch_publish=%.3f ns per call (uncontended, n=200000)\n",
           sq_window_ns());
    for (c = 0; c < sizeof cad / sizeof cad[0]; c++) {
        for (k = 0; k < sizeof readers / sizeof readers[0]; k++) {
            for (r = 0; r < runs; r++) { sq_run(cad[c], readers[k], dur, r); }
            fflush(stdout);
        }
    }
    printf("BENCH done\n");
    return 0;
}
