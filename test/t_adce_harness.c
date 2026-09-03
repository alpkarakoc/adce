/* Integration harness: the ingress site itself, under test.
 *
 * Every other test file in this repository tests a function. This one tests a
 * CALL ORDER, which is the one thing no unit test can reach -- docs/
 * enforcement-plane.md section 5 says so explicitly: "no function is wrong;
 * adce_obs_tap and adce_enf_admit both behave correctly in either order."
 * What distinguishes the two orders is a consequence visible only from a site
 * that has both, so this file builds the site.
 *
 * The runner and main() live in t_adce_platform.c; each case here is static so
 * the ran-tests guard in scripts/verify.sh finds it by source pattern, with one
 * external forwarder at the bottom. */

#include "../include/adce_enforce.h"
#include "../include/adce_obs_thread.h"

#include <stdio.h>

#define ADCE_TEST_ASSERT(cond)                                               \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
            return 1;                                                        \
        }                                                                    \
    } while (0)

/* =====================================================================
 * The instrumented ingress site.
 * ===================================================================== */

typedef struct {
    /* The real Observation Plane counter this site taps. Shared across ingress
     * threads, exactly as a deployment would share it. */
    adce_obs_counter_t *counter;

    /* Per-thread by the rule in enforcement-plane.md section 3: the bucket has
     * mutable state and no lock is permitted on this path. */
    adce_enf_ctx_t enf;

    /* Audit counter, incremented on the line immediately after the tap and
     * nowhere else, so it counts executions of the tap statement. It exists
     * because the observer thread RESETS counter->arrivals once per epoch --
     * adce_obs_counter_take is an exchange -- so under a running observer the
     * counter's final value is the last epoch's arrivals, not the run's. That
     * is why section 5 scopes the exact identity to "a run with no concurrent
     * publication". The two non-concurrent cases below assert
     * tapped == counter->arrivals directly, which is what licenses using it as
     * the tap's stand-in in the concurrent case. */
    uint64_t tapped;

    uint64_t work_done;
    uint64_t work_checksum;
} harness_site_t;

/* Real work, deliberately not an empty body. A no-op would let the optimiser
 * delete the admitted branch entirely, and a harness whose "work" costs
 * nothing cannot distinguish a path that reaches it from one that does not. */
static void harness_work(harness_site_t *site) {
    site->work_done++;
    /* Unsigned, so the wraparound is defined and UBSan stays quiet on it. */
    site->work_checksum =
        site->work_checksum * 6364136223846793005ULL + site->work_done;
}

/* THE CORRECT ORDER: tap -> gate -> work, transcribed from the ingress recipe
 * in docs/enforcement-plane.md section 3. The tap is unconditional and first,
 * and nothing returns before it. */
static void ingress_correct(harness_site_t *site, uint64_t now_ns) {
    adce_obs_tap(site->counter);
    site->tapped++;

    if (adce_enf_admit(&site->enf, now_ns) != ADCE_ENF_ADMIT) {
        return;
    }

    harness_work(site);
}

/* THE INVERTED ORDER: gate -> tap -> work. This is the "shape to reject in
 * review" from the same section, reproduced here as executable code rather
 * than as a comment, because a harness that only ever runs the correct
 * ordering proves nothing about ordering.
 *
 * It compiles, it is not obviously wrong at a glance, and every unit test in
 * this repository still passes with it in place. The only thing that catches
 * it is the identity below. */
static void ingress_inverted(harness_site_t *site, uint64_t now_ns) {
    if (adce_enf_admit(&site->enf, now_ns) != ADCE_ENF_ADMIT) {
        return;
    }

    adce_obs_tap(site->counter);
    site->tapped++;

    harness_work(site);
}

/* Every arrival the gate accounted for, by whichever of the three outcomes. */
static uint64_t site_gated(const harness_site_t *site) {
    return site->enf.admitted + site->enf.dropped_shed + site->enf.dropped_limit;
}

/* THE IDENTITY. docs/enforcement-plane.md section 5:
 *
 *     "over a run with no concurrent publication,
 *      arrivals_counted == admitted + dropped. If the tap sat after the gate,
 *      arrivals_counted == admitted and the dropped requests would be missing
 *      from the count."
 *
 * One function, used by both the correct and the inverted case, so the two are
 * measured by the same instrument and the difference between them cannot be an
 * artifact of measuring them differently. */
static int site_identity_holds(const harness_site_t *site) {
    return site->tapped == site_gated(site);
}

static void harness_report(const char *label, const harness_site_t *site) {
    printf("  HARNESS %-8s tapped=%llu admitted=%llu shed=%llu limit=%llu"
           " | %llu == %llu : %s\n",
           label, (unsigned long long)site->tapped,
           (unsigned long long)site->enf.admitted,
           (unsigned long long)site->enf.dropped_shed,
           (unsigned long long)site->enf.dropped_limit,
           (unsigned long long)site->tapped,
           (unsigned long long)site_gated(site),
           site_identity_holds(site) ? "HOLDS" : "VIOLATED");
}

static void harness_sleep_ns(uint64_t ns) {
    struct timespec req;
    req.tv_sec = (time_t)(ns / 1000000000ULL);
    req.tv_nsec = (long)(ns % 1000000000ULL);
    (void)nanosleep(&req, NULL);
}

/* =====================================================================
 * Deterministic fixture for the two ordering cases.
 *
 * The drops must be produced exactly, not statistically, or the gate becomes
 * flaky. Publishing pressure 0 achieves that without stubbing the RNG: the
 * shed test is (draw >> 48) < 0, which is false for EVERY draw. So
 * adce_enf_admit still draws from the ingress TU's real adce_rng_tls on every
 * arrival -- the real path, per section 1.3 -- and the draw provably cannot
 * change the outcome. Every drop below therefore comes from the token bucket,
 * whose count is exact.
 *
 * Offering twice the bucket's contents at a FROZEN now_ns gives elapsed == 0
 * on every refill, so the bucket never regains a token: the first
 * HARNESS_BUCKET_ARRIVALS admit and the rest are DROP_LIMIT, in every profile
 * and on every host.
 * ===================================================================== */

#define HARNESS_BUCKET_ARRIVALS (ADCE_ENF_CAPACITY_Q16 / ADCE_ENF_COST_Q16)
#define HARNESS_OFFERED (HARNESS_BUCKET_ARRIVALS * 2)

static void harness_open(harness_site_t *site, adce_obs_counter_t *counter,
                         adce_epoch_state_t *epoch, uint64_t now_ns) {
    memset(counter, 0, sizeof(*counter));
    memset(epoch, 0, sizeof(*epoch));

    /* Pressure 0, observed now, so adce_epoch_is_stale is false and the gate
     * takes the published-value branch rather than the ADCE_ENF_STALE_PRESSURE
     * fallback -- which would shed at one half and reintroduce the RNG. */
    adce_epoch_publish(epoch, ADCE_PRESSURE_MIN, 1, now_ns);

    memset(site, 0, sizeof(*site));
    site->counter = counter;
    adce_enf_thread_init(&site->enf, epoch, now_ns);
}

/* The deterministic split both ordering cases are checked against, asserted
 * once here so neither case can drift from the other's fixture. */
static int harness_check_gate_split(const harness_site_t *site) {
    ADCE_TEST_ASSERT(site->enf.admitted == HARNESS_BUCKET_ARRIVALS);
    ADCE_TEST_ASSERT(site->enf.dropped_limit == HARNESS_OFFERED - HARNESS_BUCKET_ARRIVALS);
    ADCE_TEST_ASSERT(site->enf.dropped_shed == 0);
    ADCE_TEST_ASSERT(site->enf.stale_reads == 0);
    ADCE_TEST_ASSERT(site_gated(site) == HARNESS_OFFERED);
    ADCE_TEST_ASSERT(site->work_done == site->enf.admitted);
    return 0;
}

static int test_harness_tap_before_gate(void) {
    static adce_obs_counter_t counter;
    static adce_epoch_state_t epoch;
    harness_site_t site;
    uint64_t now_ns = adce_now_ns();
    uint64_t i;

    harness_open(&site, &counter, &epoch, now_ns);

    for (i = 0; i < HARNESS_OFFERED; ++i) {
        ingress_correct(&site, now_ns);
    }

    harness_report("correct", &site);

    ADCE_TEST_ASSERT(harness_check_gate_split(&site) == 0);

    /* The audit counter is a faithful stand-in for the tap. Nothing drains
     * counter.arrivals in this test -- no observer is running -- so the real
     * Observation Plane counter and the audit counter must agree exactly. This
     * is what makes the audit counter usable in the concurrent case, where the
     * observer's per-epoch exchange makes the real one unreadable as a total. */
    ADCE_TEST_ASSERT(atomic_load_explicit(&counter.arrivals,
                                          memory_order_relaxed) == site.tapped);

    /* THE ASSERTION. Every arrival was seen by the detector, including the
     * 4096 the gate dropped. */
    ADCE_TEST_ASSERT(site.tapped == HARNESS_OFFERED);
    ADCE_TEST_ASSERT(site_identity_holds(&site));

    return 0;
}

/* The counter-proof. This case PASSES by observing the identity FAIL.
 *
 * The same fixture, the same offered load, the same identity function -- only
 * the call order differs. If this ever starts holding, the identity has lost
 * its teeth and the case above has stopped proving anything, which is why the
 * failure is asserted rather than merely commented. */
static int test_harness_tap_after_gate(void) {
    static adce_obs_counter_t counter;
    static adce_epoch_state_t epoch;
    harness_site_t site;
    uint64_t now_ns = adce_now_ns();
    uint64_t i;

    harness_open(&site, &counter, &epoch, now_ns);

    for (i = 0; i < HARNESS_OFFERED; ++i) {
        ingress_inverted(&site, now_ns);
    }

    harness_report("INVERTED", &site);

    /* The gate behaved identically: same admits, same drops. Nothing about the
     * enforcement decision changed, which is the point -- the defect is
     * entirely in what the detector was allowed to see. */
    ADCE_TEST_ASSERT(harness_check_gate_split(&site) == 0);

    /* The identity is broken, and broken in exactly the shape section 5
     * predicts: arrivals_counted == admitted, not admitted + dropped. */
    ADCE_TEST_ASSERT(!site_identity_holds(&site));
    ADCE_TEST_ASSERT(site.tapped == site.enf.admitted);
    ADCE_TEST_ASSERT(site_gated(&site) - site.tapped == site.enf.dropped_limit);
    ADCE_TEST_ASSERT(site.enf.dropped_limit > 0);

    /* And it is the REAL counter that is blind, not just the audit one. This
     * is the failure stated as the Observation Plane would experience it: 4096
     * arrivals happened, were dropped, and the detector's own counter reports
     * that they never occurred. Under load that is the self-soothing loop
     * docs/observation-plane.md section 2 exists to prevent -- containment
     * suppressing its own input signal. */
    ADCE_TEST_ASSERT(atomic_load_explicit(&counter.arrivals,
                                          memory_order_relaxed) ==
                     site.enf.admitted);
    ADCE_TEST_ASSERT(atomic_load_explicit(&counter.arrivals,
                                          memory_order_relaxed) <
                     HARNESS_OFFERED);

    return 0;
}

/* =====================================================================
 * Observer thread lifecycle.
 * ===================================================================== */

static int test_harness_observer_lifecycle(void) {
    static adce_obs_counter_t counter;
    static adce_epoch_state_t epoch;
    adce_obs_thread_t obs;
    adce_q16_t pressure;
    uint64_t epoch_id;
    uint64_t observed_at_ns;

    memset(&counter, 0, sizeof(counter));
    memset(&epoch, 0, sizeof(epoch));

    ADCE_TEST_ASSERT(adce_obs_thread_start(&obs, &counter, &epoch) == 1);

    /* Ten epochs of real time. Enough that the loop has certainly serviced a
     * deadline; nowhere near ADCE_OBS_WARMUP_EPOCHS. */
    harness_sleep_ns(ADCE_OBS_EPOCH_NS * 10);

    adce_obs_thread_stop(&obs);
    ADCE_TEST_ASSERT(obs.started == 0);

    /* Idempotent. pthread_join on an already-joined thread is undefined, so
     * this call is load-bearing rather than cosmetic. */
    adce_obs_thread_stop(&obs);

    /* The loop ran against a real clock and closed epochs on its own. */
    ADCE_TEST_ASSERT(obs.ctx.epochs_closed > 0);

    /* Warmup, asserted as the contract rather than as a timing expectation: a
     * host slow enough to sleep through 100 epochs would otherwise fail a
     * correct implementation. Nothing publishes before warmup completes. */
    ADCE_TEST_ASSERT(obs.ctx.publications == 0 ||
                     obs.ctx.epochs_closed >= ADCE_OBS_WARMUP_EPOCHS);

    /* And while nothing has published, the cold-start posture is unchanged by
     * the mere existence of a running observer: observed_at_ns is still 0, so
     * the watchdog still reads stale and Enforcement still holds its
     * conservative posture. No second mechanism, exactly as
     * adce_observe.h documents. */
    ADCE_TEST_ASSERT(adce_epoch_read(&epoch, &pressure, &epoch_id,
                                     &observed_at_ns) == 1);
    if (obs.ctx.publications == 0) {
        ADCE_TEST_ASSERT(observed_at_ns == 0);
        ADCE_TEST_ASSERT(adce_epoch_is_stale(observed_at_ns, adce_now_ns()) == 1);
    }

    /* Single-writer is enforced, not assumed: the observer thread's claim is
     * still held, so a second claimant is refused even after the join. */
    ADCE_TEST_ASSERT(adce_obs_claim_writer(&obs.ctx) == 0);

    return 0;
}

/* =====================================================================
 * The concurrent run. Ingress threads tapping and reading the seqlock while
 * the observer thread publishes into it -- the first time both concurrency
 * contracts are exercised against each other rather than in isolation. This
 * is the case TSan is for.
 * ===================================================================== */

#define HARNESS_INGRESS_THREADS 4
#define HARNESS_BATCH 1024
#define HARNESS_PUBLISH_WAIT_NS (15ULL * 1000ULL * 1000ULL * 1000ULL)
#define HARNESS_OVERLAP_EPOCHS 30ULL

static adce_obs_counter_t g_h_counter;
static adce_epoch_state_t g_h_epoch;
static harness_site_t g_h_sites[HARNESS_INGRESS_THREADS];
static _Atomic int g_h_stop;

static void *harness_ingress_main(void *arg) {
    harness_site_t *site = (harness_site_t *)arg;

    /* On the ingress thread, off the request path, per section 1.4: this warms
     * THIS thread's stream so an entropy failure aborts here rather than on an
     * arrival. It memsets only site->enf, so site->counter set by the starting
     * thread survives it. */
    adce_enf_thread_init(&site->enf, &g_h_epoch, adce_now_ns());

    while (!atomic_load_explicit(&g_h_stop, memory_order_acquire)) {
        int b;
        for (b = 0; b < HARNESS_BATCH; ++b) {
            /* A fresh clock read per arrival, serving both the staleness check
             * and the bucket refill -- one read, two uses, per section 2. */
            ingress_correct(site, adce_now_ns());
        }
        /* Paces the offered load so the run spans real epochs instead of
         * saturating four cores; the arrival rate still comfortably exceeds
         * the per-thread bucket rate, so all three outcomes occur. */
        harness_sleep_ns(1000ULL * 1000ULL);
    }

    return NULL;
}

static int test_harness_concurrent(void) {
    adce_obs_thread_t obs;
    pthread_t ingress[HARNESS_INGRESS_THREADS];
    size_t i;
    uint64_t deadline_ns;
    uint64_t started_ns;
    uint64_t elapsed_ns;
    uint64_t ceiling;
    uint64_t total_tapped = 0;
    uint64_t total_gated = 0;
    uint64_t residual;
    adce_q16_t pressure = 0;
    uint64_t epoch_id = 0;
    uint64_t observed_at_ns = 0;
    int published = 0;

    memset(&g_h_counter, 0, sizeof(g_h_counter));
    memset(&g_h_epoch, 0, sizeof(g_h_epoch));
    atomic_store_explicit(&g_h_stop, 0, memory_order_release);

    for (i = 0; i < HARNESS_INGRESS_THREADS; ++i) {
        memset(&g_h_sites[i], 0, sizeof(g_h_sites[i]));
        /* One counter, many tappers: the many-writer contract the padding in
         * adce_observe.h exists for. */
        g_h_sites[i].counter = &g_h_counter;
    }

    ADCE_TEST_ASSERT(adce_obs_thread_start(&obs, &g_h_counter, &g_h_epoch) == 1);

    /* Read before the threads exist, so every bucket's refill window is a
     * subset of [started_ns, elapsed]. Over-measuring elapsed only weakens the
     * ceiling assertion below; under-measuring it would make the assertion
     * lie. */
    started_ns = adce_now_ns();

    for (i = 0; i < HARNESS_INGRESS_THREADS; ++i) {
        ADCE_TEST_ASSERT(pthread_create(&ingress[i], NULL,
                                        harness_ingress_main,
                                        &g_h_sites[i]) == 0);
    }

    /* Wait for a real publication, watched through adce_epoch_read. Polling
     * obs.ctx.publications instead would be a data race on a non-atomic field
     * the observer thread is writing; the seqlock is the race-free way to
     * observe the observer's progress, and this poll is a third concurrent
     * reader while it does so.
     *
     * The wait is bounded rather than open-ended because warmup is 100 epochs
     * of real time, and a hung observer must fail the test rather than hang
     * the gate. */
    deadline_ns = adce_now_ns() + HARNESS_PUBLISH_WAIT_NS;
    while (adce_now_ns() < deadline_ns) {
        if (adce_epoch_read(&g_h_epoch, &pressure, &epoch_id,
                            &observed_at_ns) &&
            epoch_id != 0) {
            published = 1;
            break;
        }
        harness_sleep_ns(1000ULL * 1000ULL);
    }

    /* Keep tapping AFTER the first publication. Stopping on it would leave the
     * ingress threads overlapping exactly one seqlock write, which is thin
     * evidence for the case that exists to produce it: the whole point here is
     * many publications landing while many threads read the same line. This
     * window is worth about HARNESS_OVERLAP_EPOCHS more of them. */
    if (published) {
        harness_sleep_ns(ADCE_OBS_EPOCH_NS * HARNESS_OVERLAP_EPOCHS);
    }

    atomic_store_explicit(&g_h_stop, 1, memory_order_release);
    for (i = 0; i < HARNESS_INGRESS_THREADS; ++i) {
        ADCE_TEST_ASSERT(pthread_join(ingress[i], NULL) == 0);
    }
    adce_obs_thread_stop(&obs);
    elapsed_ns = adce_now_ns() - started_ns;

    /* Both planes actually ran. Without this the identity below could pass on
     * a run where the observer never published and nothing was concurrent. */
    ADCE_TEST_ASSERT(published == 1);
    /* More than one, so the concurrency this case exists to exercise is a
     * sustained overlap rather than a single lucky instant. */
    ADCE_TEST_ASSERT(obs.ctx.publications > 1);
    ADCE_TEST_ASSERT(obs.ctx.epochs_closed >= ADCE_OBS_WARMUP_EPOCHS);
    ADCE_TEST_ASSERT(observed_at_ns != 0);

    /* Published pressure honours the publication contract after crossing the
     * seqlock under live contention. */
    ADCE_TEST_ASSERT(pressure >= ADCE_PRESSURE_MIN);
    ADCE_TEST_ASSERT(pressure <= ADCE_PRESSURE_MAX);

    for (i = 0; i < HARNESS_INGRESS_THREADS; ++i) {
        /* THE ASSERTION, per ingress thread. Reading these after the join is
         * race-free: pthread_join is the synchronisation edge. */
        ADCE_TEST_ASSERT(site_identity_holds(&g_h_sites[i]));
        ADCE_TEST_ASSERT(g_h_sites[i].tapped > 0);
        ADCE_TEST_ASSERT(g_h_sites[i].work_done == g_h_sites[i].enf.admitted);
        total_tapped += g_h_sites[i].tapped;
        total_gated += site_gated(&g_h_sites[i]);
    }

    ADCE_TEST_ASSERT(total_tapped == total_gated);

    /* The absolute ceiling holds per thread, under live concurrency and a real
     * clock. This is section 7 item 6 carried into the concurrent case, and it
     * is deliberately phrased as a BOUND rather than as an expected drop count:
     * enforcement-plane.md section 1.2 records that no test may assert the
     * values of ADCE_ENF_RATE_Q16_PER_NS or ADCE_ENF_CAPACITY_Q16, so that
     * retuning them cannot make a test lie about the property being protected.
     * An assertion that DROP_LIMIT occurred would be exactly such a test -- it
     * would hold or fail on whether the harness happened to offer load above
     * the configured rate, which is a fact about the harness, not the gate.
     *
     * The bucket starts full, so capacity is the burst allowance on top of
     * rate * elapsed. Summed over the threads this is the "aggregate ceiling
     * is threads * rate" cost that section 3 insists must not be discovered
     * later. */
    ceiling = (ADCE_ENF_RATE_Q16_PER_NS * elapsed_ns + ADCE_ENF_CAPACITY_Q16) /
              ADCE_ENF_COST_Q16;

    /* The stale posture and the admit path both ran. Neither is rate-dependent:
     * warmup suppresses publication for a full second, so every arrival before
     * the first publication reads stale and sheds at ADCE_ENF_STALE_PRESSURE,
     * and the bucket starts full so the opening arrivals admit. */
    {
        uint64_t admitted = 0, shed = 0, limit = 0, stale = 0;
        for (i = 0; i < HARNESS_INGRESS_THREADS; ++i) {
            ADCE_TEST_ASSERT(g_h_sites[i].enf.admitted <= ceiling);
            admitted += g_h_sites[i].enf.admitted;
            shed += g_h_sites[i].enf.dropped_shed;
            limit += g_h_sites[i].enf.dropped_limit;
            stale += g_h_sites[i].enf.stale_reads;
        }
        ADCE_TEST_ASSERT(admitted > 0);
        ADCE_TEST_ASSERT(shed > 0);
        ADCE_TEST_ASSERT(stale > 0);
        printf("  HARNESS concurrent tapped=%llu admitted=%llu shed=%llu"
               " limit=%llu stale=%llu | ceiling/thread=%llu"
               " publications=%llu late=%llu skipped=%llu discarded=%llu\n",
               (unsigned long long)total_tapped, (unsigned long long)admitted,
               (unsigned long long)shed, (unsigned long long)limit,
               (unsigned long long)stale, (unsigned long long)ceiling,
               (unsigned long long)obs.ctx.publications,
               (unsigned long long)obs.late_epochs,
               (unsigned long long)obs.skipped_epochs,
               (unsigned long long)obs.discarded_arrivals);
    }

    /* Conservation across the observer boundary. The observer drained the
     * counter once per epoch, so its final value is not the run's total and
     * the exact identity is unavailable here -- which is precisely why
     * section 5 scoped that identity to a run with no concurrent publication.
     * What IS available is a bound: everything the observer discarded plus
     * whatever it had not yet taken cannot exceed what the tap put in. A
     * double tap, or a tap the site did not account for, breaks it. */
    residual = atomic_load_explicit(&g_h_counter.arrivals, memory_order_relaxed);
    ADCE_TEST_ASSERT(residual + obs.discarded_arrivals <= total_tapped);

    return 0;
}

/* =====================================================================
 * External forwarders; see the header comment.
 * ===================================================================== */

#define ADCE_HARNESS_TEST_EXPORT(name)                                       \
    int adce_t_##name(void);                                                 \
    int adce_t_##name(void) { return test_##name(); }

ADCE_HARNESS_TEST_EXPORT(harness_tap_before_gate)
ADCE_HARNESS_TEST_EXPORT(harness_tap_after_gate)
ADCE_HARNESS_TEST_EXPORT(harness_observer_lifecycle)
ADCE_HARNESS_TEST_EXPORT(harness_concurrent)
