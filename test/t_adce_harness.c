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
     * counter once per epoch, so its final value alone is not the run's
     * total -- which is why section 5 scoped the plain tapped == gated
     * identity to a run with no concurrent publication. But every tap now
     * lands exactly one of three places: folded into a closed epoch
     * (obs.ctx.arrivals_closed, warmup included -- see adce_obs_epoch_close),
     * dropped by a missed-epoch discard (obs.discarded_arrivals), or still
     * sitting in the counter at run end (residual). Reading obs.ctx.* and
     * obs.discarded_arrivals here is race-free: both are written only by the
     * observer thread, and adce_obs_thread_stop() above already joined it. */
    residual = atomic_load_explicit(&g_h_counter.arrivals, memory_order_relaxed);
    ADCE_TEST_ASSERT(total_tapped ==
                     obs.ctx.arrivals_closed + obs.discarded_arrivals + residual);

    return 0;
}

/* =====================================================================
 * The fail-closed path: what the system does when publication stops.
 *
 * docs/enforcement-plane.md section 4 makes four claims about the stale
 * window and section 5 lists "observer death mid-flight" as needing this
 * harness. Until now none of them had executable evidence.
 *
 * WHY THIS RUNS ITS OWN CLOSE LOOP instead of stalling the library observer.
 * A stall-injection hook in src/adce_obs_thread.c would be a shipped API that
 * stops publication in a production binary -- an attack surface bolted onto
 * the very defence the watchdog exists to provide, and section 4 already notes
 * that "anyone who can stall or kill it turns containment off completely".
 * So the harness drives the plane itself, calling adce_obs_epoch_close with a
 * clock it owns. That call takes observed_at_ns as a PARAMETER precisely so
 * this is possible; see adce_observe.h. The library observer thread is not
 * used here at all.
 *
 * The closer is ONE long-lived thread whose publishing is gated, rather than a
 * thread that is stopped and a second one started. adce_obs_claim_writer is a
 * one-way latch -- writer_claimed goes 0 -> 1 -> 2 and is never released, and
 * the ownership test compares pthread_self() -- so a replacement thread would
 * be refused the claim, take the -1 path on every close, and publish nothing
 * while still looking alive. Re-initialising the context instead would reset
 * epochs_closed and impose a fresh 100-epoch warmup blackout on recovery,
 * which would test the cold start rather than the recovery. Gating the
 * publication stops publication, which is the variable under test.
 * ===================================================================== */

#define HARNESS_STALE_THREADS 4
#define HARNESS_STALE_BATCH 256
#define HARNESS_STALE_PACE_NS (250ULL * 1000ULL)
#define HARNESS_CLOSER_TICK_NS (1000ULL * 1000ULL)

#define HARNESS_SETTLE_NS (ADCE_OBS_EPOCH_NS * 20ULL)
#define HARNESS_LIVE_NS (ADCE_OBS_EPOCH_NS * 30ULL)
/* The advice timeout plus margin: the window in which the last publication is
 * ageing but has not yet crossed ADCE_ADVICE_TIMEOUT_NS. Reads here are fresh
 * then stale, so it is measured for the ceiling and asserted for nothing else. */
#define HARNESS_AGING_NS (ADCE_ADVICE_TIMEOUT_NS + ADCE_OBS_EPOCH_NS * 3ULL)
#define HARNESS_BLIND_SLICE_NS (ADCE_OBS_EPOCH_NS * 12ULL)
#define HARNESS_RECOVER_NS (ADCE_OBS_EPOCH_NS * 30ULL)
#define HARNESS_STALE_WAIT_NS (15ULL * 1000ULL * 1000ULL * 1000ULL)

/* Shedding under ADCE_ENF_STALE_PRESSURE is exactly p = 1/2: the test is
 * (draw >> 48) < 32768 over 65536 uniform buckets. Asserted as a band rather
 * than a point because the draws are real -- this is the ingress TU's actual
 * adce_rng_tls, which no test can seed (section 1.3). At roughly 30k arrivals
 * per thread per slice the band below is over four sigma wide. */
#define HARNESS_STALE_BAND_LO 420ULL
#define HARNESS_STALE_BAND_HI 580ULL
/* The live band. Published pressure under steady load settles at zero, so the
 * shed fraction is essentially nil; the allowance is for the occasional
 * legitimate z > z_lo excursion from scheduler jitter in the arrival rate. It
 * is deliberately far below the stale band, and the separation is asserted
 * too, so no run can satisfy both. */
#define HARNESS_LIVE_BAND_HI 250ULL
#define HARNESS_BAND_SEPARATION 150ULL

enum {
    HARNESS_PH_PRIME = 0,  /* warmup; nothing published yet, nothing asserted */
    HARNESS_PH_LIVE,       /* publishing and settled */
    HARNESS_PH_AGING,      /* publication frozen, still inside the timeout */
    HARNESS_PH_BLIND_A,
    HARNESS_PH_BLIND_B,
    HARNESS_PH_BLIND_C,
    /* The thaw is its own window for the same reason the ageing one is: from
     * enabling the closer to observing a fresh publication, reads cross back
     * from stale to fresh. Arrivals in there belong to neither posture, so
     * they are measured for the ceiling and asserted for nothing else.
     * Without it, the recovered arrivals landed inside BLIND_C's delta and
     * broke "every read in a blind phase is stale" -- correctly. */
    HARNESS_PH_THAWING,
    HARNESS_PH_RECOVERED,
    HARNESS_PH_END,        /* sentinel: final snapshot, then the loop exits */
    HARNESS_PH_COUNT
};

typedef struct {
    uint64_t at_ns;
    uint64_t tapped;
    uint64_t admitted;
    uint64_t shed;
    uint64_t limit;
    uint64_t stale;
    /* The stale total split by route (section 4.1). Carried per phase because
     * the routes have different bounds: torn and future are bounded by
     * publications in the window, aged only by arrivals. */
    uint64_t torn;
    uint64_t future;
    uint64_t aged;
} harness_snap_t;

/* Composition rather than extra fields on harness_site_t: the ordering cases
 * above share that type and have no phases. */
typedef struct {
    harness_site_t site;
    harness_snap_t snap[HARNESS_PH_COUNT];

    /* Atomic because the driving thread WAITS on it. A phase boundary that the
     * driver only announces, without confirming it was recorded, is not a
     * boundary -- see harness_phase_sync. */
    _Atomic int observed_phase;
} harness_phased_t;

static adce_obs_counter_t g_st_counter;
static adce_epoch_state_t g_st_epoch;
static adce_obs_ctx_t g_st_obs;
static harness_phased_t g_st_sites[HARNESS_STALE_THREADS];

static _Atomic int g_st_phase;
static _Atomic int g_st_closer_run;
static _Atomic int g_st_closing;  /* the gate: 1 publishes, 0 freezes */
static _Atomic int g_st_thaw;     /* set with g_st_closing on resume */
static _Atomic int g_st_ready;

/* The thaw-drain term of this test's own overrun identity. This is harness
 * policy, not library policy -- adce_obs_ctx_t has no equivalent, because the
 * shipped observer (src/adce_obs_thread.c) never drops a backlog without
 * counting it as discarded_arrivals. Here the drain is a single line inside
 * harness_closer_main, so a plain counter suffices: written only by the
 * closer thread, read only after pthread_join(closer, ...) below. */
static uint64_t g_st_thaw_discarded;

static void harness_snap_take(harness_snap_t *snap, const harness_site_t *site) {
    snap->at_ns = adce_now_ns();
    snap->tapped = site->tapped;
    snap->admitted = site->enf.admitted;
    snap->shed = site->enf.dropped_shed;
    snap->limit = site->enf.dropped_limit;
    snap->stale = site->enf.stale_reads;
    snap->torn = site->enf.torn_reads;
    snap->future = site->enf.future_reads;
    snap->aged = site->enf.aged_reads;
}

static uint64_t harness_permille(uint64_t part, uint64_t whole) {
    return whole == 0 ? 0 : (part * 1000ULL) / whole;
}

/* The harness's own Observation Plane driver. Everything it does with the
 * plane is one call; the rest is cadence, which is the half this test needs to
 * control. */
static void *harness_closer_main(void *arg) {
    uint64_t deadline_ns;

    (void)arg;

    /* On this thread, because the claim records pthread_self() as owner. */
    if (!adce_obs_claim_writer(&g_st_obs)) {
        atomic_store_explicit(&g_st_ready, -1, memory_order_release);
        return NULL;
    }

    deadline_ns = adce_now_ns() + ADCE_OBS_EPOCH_NS;
    atomic_store_explicit(&g_st_ready, 1, memory_order_release);

    while (atomic_load_explicit(&g_st_closer_run, memory_order_acquire)) {
        uint64_t now_ns = adce_now_ns();

        if (now_ns >= deadline_ns) {
            if (atomic_load_explicit(&g_st_closing, memory_order_acquire)) {
                /* On thaw, drop the backlog before closing anything. Nothing
                 * drained the counter while publication was frozen, so it now
                 * holds several hundred milliseconds of arrivals; handing that
                 * to one epoch would divide a multi-epoch total by a single T
                 * and manufacture a rate spike out of the freeze itself,
                 * saturating the squash and holding pressure at maximum for
                 * the whole of the recovery this test is trying to measure.
                 * This is the same missed-epoch policy the shipped observer
                 * applies in src/adce_obs_thread.c, applied for the same
                 * reason. */
                if (atomic_exchange_explicit(&g_st_thaw, 0,
                                             memory_order_acq_rel)) {
                    g_st_thaw_discarded += adce_obs_counter_take(g_st_obs.counter);
                }
                (void)adce_obs_epoch_close(&g_st_obs, now_ns);
            }
            deadline_ns = now_ns + ADCE_OBS_EPOCH_NS;
        }

        harness_sleep_ns(HARNESS_CLOSER_TICK_NS);
    }

    return NULL;
}

static void *harness_stale_ingress_main(void *arg) {
    harness_phased_t *ps = (harness_phased_t *)arg;
    int mine = HARNESS_PH_PRIME;

    adce_enf_thread_init(&ps->site.enf, &g_st_epoch, adce_now_ns());
    harness_snap_take(&ps->snap[HARNESS_PH_PRIME], &ps->site);
    atomic_store_explicit(&ps->observed_phase, mine, memory_order_release);

    for (;;) {
        int phase = atomic_load_explicit(&g_st_phase, memory_order_acquire);
        int b;

        /* One snapshot per phase crossed, even if several were crossed between
         * two batches. A phase this thread stepped straight over then shows a
         * zero-length delta, and the assertions below reject a zero-arrival
         * phase rather than passing it vacuously. */
        while (mine < phase) {
            mine++;
            harness_snap_take(&ps->snap[mine], &ps->site);
            /* Published AFTER the snapshot, and with release, so a driver that
             * observes this value knows the boundary is already recorded. */
            atomic_store_explicit(&ps->observed_phase, mine,
                                  memory_order_release);
        }

        if (mine >= HARNESS_PH_END) {
            break;
        }

        for (b = 0; b < HARNESS_STALE_BATCH; ++b) {
            ingress_correct(&ps->site, adce_now_ns());
        }
        harness_sleep_ns(HARNESS_STALE_PACE_NS);
    }

    return NULL;
}

/* Advances the phase and does not return until EVERY ingress thread has
 * recorded the boundary.
 *
 * Announcing a phase is not the same as the phase having started, and the
 * difference is not cosmetic. The threads observe g_st_phase between batches,
 * so a thread can still be accumulating into the previous phase's delta for a
 * batch plus a pace-sleep after the store. If the driver changes the system in
 * that window -- freezing or resuming publication -- those arrivals land in the
 * wrong phase and are measured against the wrong expectation. That is what made
 * a blind slice report an arrival that had not read stale: publication had
 * already resumed while a thread was still inside BLIND_C.
 *
 * Adding HARNESS_PH_THAWING narrowed that window but could not close it, because
 * the store and the thaw are two separate operations with no ordering between
 * the threads and the driver. Waiting for the acknowledgement closes it. */
static void harness_phase_sync(int phase) {
    size_t k;

    atomic_store_explicit(&g_st_phase, phase, memory_order_release);

    for (k = 0; k < HARNESS_STALE_THREADS; ++k) {
        while (atomic_load_explicit(&g_st_sites[k].observed_phase,
                                    memory_order_acquire) < phase) {
            harness_sleep_ns(HARNESS_CLOSER_TICK_NS);
        }
    }
}

/* Separate from the sampling helper below because the ageing phase is measured
 * but not asserted. */
static void harness_phase_hold(int phase, uint64_t dur_ns) {
    harness_phase_sync(phase);
    harness_sleep_ns(dur_ns);
}

/* Holds a phase while continuously checking, through the seqlock, that the
 * published epoch has not moved and still reads stale. This is requirement 1
 * as a sustained property rather than a single sample: "becomes true and STAYS
 * true". The poll is also a third concurrent reader against the ingress
 * threads. */
static int harness_phase_watch_stale(int phase, uint64_t dur_ns,
                                     uint64_t frozen_at_ns) {
    uint64_t end_ns;

    harness_phase_sync(phase);
    end_ns = adce_now_ns() + dur_ns;

    while (adce_now_ns() < end_ns) {
        adce_q16_t pressure;
        uint64_t epoch_id;
        uint64_t observed_at_ns;

        if (adce_epoch_read(&g_st_epoch, &pressure, &epoch_id,
                            &observed_at_ns)) {
            /* Publication really has stopped: the timestamp has not advanced
             * since the freeze. */
            ADCE_TEST_ASSERT(observed_at_ns == frozen_at_ns);
            ADCE_TEST_ASSERT(adce_epoch_is_stale(observed_at_ns,
                                                 adce_now_ns()) == 1);
        }
        harness_sleep_ns(HARNESS_CLOSER_TICK_NS);
    }

    return 0;
}

/* Per-phase deltas for one site. */
static uint64_t harness_delta_arrivals(const harness_phased_t *ps, int phase) {
    return ps->snap[phase + 1].tapped - ps->snap[phase].tapped;
}

static uint64_t harness_delta_shed(const harness_phased_t *ps, int phase) {
    return ps->snap[phase + 1].shed - ps->snap[phase].shed;
}

static uint64_t harness_delta_stale(const harness_phased_t *ps, int phase) {
    return ps->snap[phase + 1].stale - ps->snap[phase].stale;
}

static uint64_t harness_delta_torn(const harness_phased_t *ps, int phase) {
    return ps->snap[phase + 1].torn - ps->snap[phase].torn;
}

static uint64_t harness_delta_future(const harness_phased_t *ps, int phase) {
    return ps->snap[phase + 1].future - ps->snap[phase].future;
}

static uint64_t harness_delta_aged(const harness_phased_t *ps, int phase) {
    return ps->snap[phase + 1].aged - ps->snap[phase].aged;
}

/* An upper bound on how many publications a phase could have contained. The
 * closer sets its next deadline to now + T after every close, so its cadence is
 * at least T and a window of duration D holds at most D/T of them; the +1
 * covers the partial epoch at each edge. */
static uint64_t harness_publications_bound(const harness_phased_t *ps,
                                           int phase) {
    uint64_t dur_ns = ps->snap[phase + 1].at_ns - ps->snap[phase].at_ns;
    return dur_ns / ADCE_OBS_EPOCH_NS + 1;
}

/* The absolute ceiling over an arbitrary window: rate * elapsed, plus one full
 * bucket for the burst the window may have opened with. Independent of
 * pressure by design (section 1.2), which is exactly why it is the thing that
 * still bounds volume while the detector is blind. */
static uint64_t harness_ceiling(const harness_phased_t *ps, int from, int to) {
    uint64_t elapsed_ns = ps->snap[to].at_ns - ps->snap[from].at_ns;
    return (ADCE_ENF_RATE_Q16_PER_NS * elapsed_ns + ADCE_ENF_CAPACITY_Q16) /
           ADCE_ENF_COST_Q16;
}

static uint64_t harness_delta_admitted(const harness_phased_t *ps, int from,
                                       int to) {
    return ps->snap[to].admitted - ps->snap[from].admitted;
}

/* A live phase: the detector is publishing, so nothing reads stale and the
 * shed fraction sits in the live band. */
static int harness_check_live_phase(const harness_phased_t *ps, int phase,
                                    const char *label) {
    uint64_t arrivals = harness_delta_arrivals(ps, phase);
    uint64_t permille;

    /* A phase this thread produced nothing in would satisfy every band below
     * vacuously, so an empty phase is a failure rather than a pass. */
    ADCE_TEST_ASSERT(arrivals > 0);

    /* Requirement 2, the "was zero before it" half -- but bounded by
     * publications rather than asserted at exactly zero, and the difference is
     * a measurement, not a convenience.
     *
     * Instrumenting adce_enf_decide to classify every live-phase stale read
     * showed the aged count is zero: across runs, not one of them came from an
     * epoch that had actually crossed ADCE_ADVICE_TIMEOUT_NS, and the maximum
     * age observed on the stale branch was 0. Close-to-close cadence peaked at
     * 11.2 ms against a 50 ms timeout, so publication never lapsed. The handful
     * that do occur have exactly two causes, both of them this design working:
     *
     *   - a TORN read. adce_epoch_read returned 0 because the reader landed
     *     inside the writer's sequence window, and adce_enf_decide treats that
     *     as stale on purpose -- "no snapshot means no advice, which is the
     *     conservative reading".
     *   - a publication landing between the caller's adce_now_ns() and the
     *     epoch read inside the gate, which makes observed_at_ns exceed the
     *     now_ns already captured. adce_epoch_is_stale subtracts those as
     *     uint64_t, so it underflows to an enormous age and reads stale.
     *
     * Both are fail-closed and both require a CONCURRENT PUBLICATION, and a
     * sequential thread can straddle at most one publication at a time. So the
     * count is bounded by publications in the window, never by arrivals -- it
     * does not grow with offered load, which is what distinguishes it from the
     * posture actually engaging. Asserting == 0 would be asserting that the
     * seqlock never tears and that no publish ever lands in the read window,
     * neither of which this design claims. */
    ADCE_TEST_ASSERT(harness_delta_stale(ps, phase) <=
                     harness_publications_bound(ps, phase));

    /* And the same fact stated against the load: to the resolution the run
     * measures, still zero -- against 1000 for a blind phase. */
    ADCE_TEST_ASSERT(harness_permille(harness_delta_stale(ps, phase),
                                      arrivals) == 0);

    permille = harness_permille(harness_delta_shed(ps, phase), arrivals);
    if (permille > HARNESS_LIVE_BAND_HI) {
        fprintf(stderr, "FAIL: %s shed=%llu permille exceeds live band\n",
                label, (unsigned long long)permille);
        return 1;
    }

    return 0;
}

/* A blind phase: publication has stopped and the timeout has passed, so every
 * read is stale and ADCE_ENF_STALE_PRESSURE is what the gate sheds on. */
static int harness_check_blind_phase(const harness_phased_t *ps, int phase,
                                     const char *label) {
    uint64_t arrivals = harness_delta_arrivals(ps, phase);
    uint64_t permille;

    ADCE_TEST_ASSERT(arrivals > 0);

    /* Requirement 2, the "climbs once the timeout passes" half. Every arrival,
     * not merely some: the window is entirely past ADCE_ADVICE_TIMEOUT_NS. */
    ADCE_TEST_ASSERT(harness_delta_stale(ps, phase) == arrivals);

    /* Requirement 3. The band, not a point value. */
    permille = harness_permille(harness_delta_shed(ps, phase), arrivals);
    if (permille < HARNESS_STALE_BAND_LO || permille > HARNESS_STALE_BAND_HI) {
        fprintf(stderr, "FAIL: %s shed=%llu permille outside stale band\n",
                label, (unsigned long long)permille);
        return 1;
    }

    /* Requirement 4, per slice. The stale posture sheds probabilistically, so
     * within the blind window this bucket is the only thing bounding absolute
     * volume -- and it does not consult the epoch state at all, which is what
     * makes a non-maximal fallback safe (section 4). */
    ADCE_TEST_ASSERT(harness_delta_admitted(ps, phase, phase + 1) <=
                     harness_ceiling(ps, phase, phase + 1));

    return 0;
}

static int test_harness_stale_posture(void) {
    pthread_t closer;
    pthread_t ingress[HARNESS_STALE_THREADS];
    size_t i;
    int ready;
    int published = 0;
    int recovered = 0;
    uint64_t deadline_ns;
    uint64_t frozen_at_ns = 0;
    adce_q16_t pressure = 0;
    uint64_t epoch_id = 0;
    uint64_t observed_at_ns = 0;
    uint64_t total_tapped = 0;
    uint64_t residual;

    memset(&g_st_counter, 0, sizeof(g_st_counter));
    memset(&g_st_epoch, 0, sizeof(g_st_epoch));
    adce_obs_init(&g_st_obs, &g_st_counter, &g_st_epoch);
    g_st_thaw_discarded = 0;

    for (i = 0; i < HARNESS_STALE_THREADS; ++i) {
        memset(&g_st_sites[i], 0, sizeof(g_st_sites[i]));
        g_st_sites[i].site.counter = &g_st_counter;
    }

    atomic_store_explicit(&g_st_phase, HARNESS_PH_PRIME, memory_order_release);
    atomic_store_explicit(&g_st_closer_run, 1, memory_order_release);
    atomic_store_explicit(&g_st_closing, 1, memory_order_release);
    atomic_store_explicit(&g_st_thaw, 0, memory_order_release);
    atomic_store_explicit(&g_st_ready, 0, memory_order_release);

    ADCE_TEST_ASSERT(pthread_create(&closer, NULL, harness_closer_main,
                                    NULL) == 0);
    while ((ready = atomic_load_explicit(&g_st_ready,
                                         memory_order_acquire)) == 0) {
        harness_sleep_ns(HARNESS_CLOSER_TICK_NS);
    }
    ADCE_TEST_ASSERT(ready == 1);

    for (i = 0; i < HARNESS_STALE_THREADS; ++i) {
        ADCE_TEST_ASSERT(pthread_create(&ingress[i], NULL,
                                        harness_stale_ingress_main,
                                        &g_st_sites[i]) == 0);
    }

    /* Warmup: ADCE_OBS_WARMUP_EPOCHS closes before anything is published, and
     * every arrival until then reads stale -- the cold-start posture, which is
     * why HARNESS_PH_PRIME is measured but asserted only for the ceiling. */
    deadline_ns = adce_now_ns() + HARNESS_STALE_WAIT_NS;
    while (adce_now_ns() < deadline_ns) {
        if (adce_epoch_read(&g_st_epoch, &pressure, &epoch_id,
                            &observed_at_ns) &&
            epoch_id != 0) {
            published = 1;
            break;
        }
        harness_sleep_ns(HARNESS_CLOSER_TICK_NS);
    }
    ADCE_TEST_ASSERT(published == 1);

    /* Let the EWMA settle against the offered rate before measuring the live
     * band; the epochs immediately after warmup still carry the transient. */
    harness_sleep_ns(HARNESS_SETTLE_NS);

    harness_phase_hold(HARNESS_PH_LIVE, HARNESS_LIVE_NS);

    ADCE_TEST_ASSERT(adce_epoch_read(&g_st_epoch, &pressure, &epoch_id,
                                     &observed_at_ns) == 1);
    ADCE_TEST_ASSERT(adce_epoch_is_stale(observed_at_ns, adce_now_ns()) == 0);
    frozen_at_ns = observed_at_ns;

    /* Close the LIVE window on every ingress thread BEFORE publication stops,
     * so no thread is still accumulating into the live delta when the epoch
     * starts ageing. */
    harness_phase_sync(HARNESS_PH_AGING);

    /* THE FREEZE. The closer keeps running and keeps its writer claim; it
     * simply stops calling adce_obs_epoch_close, so nothing is published and
     * observed_at_ns stops advancing. */
    atomic_store_explicit(&g_st_closing, 0, memory_order_release);

    /* Ageing: past the last publication but not yet past the advice timeout.
     * Reads cross from fresh to stale somewhere inside this window, so it is
     * asserted for the ceiling only. */
    harness_sleep_ns(HARNESS_AGING_NS);

    /* Requirement 1: stale becomes true and STAYS true, and the published
     * timestamp does not move, for the whole blind window. */
    frozen_at_ns = 0;
    ADCE_TEST_ASSERT(adce_epoch_read(&g_st_epoch, &pressure, &epoch_id,
                                     &observed_at_ns) == 1);
    frozen_at_ns = observed_at_ns;
    ADCE_TEST_ASSERT(adce_epoch_is_stale(frozen_at_ns, adce_now_ns()) == 1);

    ADCE_TEST_ASSERT(harness_phase_watch_stale(HARNESS_PH_BLIND_A,
                                               HARNESS_BLIND_SLICE_NS,
                                               frozen_at_ns) == 0);
    ADCE_TEST_ASSERT(harness_phase_watch_stale(HARNESS_PH_BLIND_B,
                                               HARNESS_BLIND_SLICE_NS,
                                               frozen_at_ns) == 0);
    ADCE_TEST_ASSERT(harness_phase_watch_stale(HARNESS_PH_BLIND_C,
                                               HARNESS_BLIND_SLICE_NS,
                                               frozen_at_ns) == 0);

    /* THE THAW. Requirement 5: a fail-closed path that cannot recover is an
     * outage, not a defence. The phase advances BEFORE publication resumes so
     * that the blind window's last slice is closed while it is still blind. */
    harness_phase_sync(HARNESS_PH_THAWING);
    atomic_store_explicit(&g_st_thaw, 1, memory_order_release);
    atomic_store_explicit(&g_st_closing, 1, memory_order_release);

    deadline_ns = adce_now_ns() + HARNESS_STALE_WAIT_NS;
    while (adce_now_ns() < deadline_ns) {
        if (adce_epoch_read(&g_st_epoch, &pressure, &epoch_id,
                            &observed_at_ns) &&
            observed_at_ns != frozen_at_ns) {
            recovered = 1;
            break;
        }
        harness_sleep_ns(HARNESS_CLOSER_TICK_NS);
    }
    ADCE_TEST_ASSERT(recovered == 1);
    ADCE_TEST_ASSERT(adce_epoch_is_stale(observed_at_ns, adce_now_ns()) == 0);

    harness_phase_hold(HARNESS_PH_RECOVERED, HARNESS_RECOVER_NS);

    atomic_store_explicit(&g_st_phase, HARNESS_PH_END, memory_order_release);
    for (i = 0; i < HARNESS_STALE_THREADS; ++i) {
        ADCE_TEST_ASSERT(pthread_join(ingress[i], NULL) == 0);
    }
    atomic_store_explicit(&g_st_closer_run, 0, memory_order_release);
    ADCE_TEST_ASSERT(pthread_join(closer, NULL) == 0);

    /* The plane really ran: warmup completed, epochs were published, and the
     * freeze did not simply stop a loop that had never done anything. */
    ADCE_TEST_ASSERT(g_st_obs.epochs_closed >= ADCE_OBS_WARMUP_EPOCHS);
    ADCE_TEST_ASSERT(g_st_obs.publications > 1);

    /* Reporting is a SEPARATE pass from checking, and deliberately so. These
     * loops were one, so the first failing site aborted the run before the
     * sites after it had printed -- exactly the diagnostic needed to tell a
     * per-thread mechanism from a global one, discarded at the moment it
     * became interesting. Every site reports, then every site is checked. */
    for (i = 0; i < HARNESS_STALE_THREADS; ++i) {
        const harness_phased_t *ps = &g_st_sites[i];
        uint64_t live_pm;
        uint64_t blind_pm;

        live_pm = harness_permille(harness_delta_shed(ps, HARNESS_PH_LIVE),
                                   harness_delta_arrivals(ps, HARNESS_PH_LIVE));
        blind_pm =
            harness_permille(harness_delta_shed(ps, HARNESS_PH_BLIND_B),
                             harness_delta_arrivals(ps, HARNESS_PH_BLIND_B));

        printf("  HARNESS stale[%d] shed permille: live=%llu blind=%llu/%llu/%llu"
               " recovered=%llu | stale_reads live=%llu blind=%llu"
               " recovered=%llu | admitted=%llu ceiling=%llu\n",
               (int)i, (unsigned long long)live_pm,
               (unsigned long long)harness_permille(
                   harness_delta_shed(ps, HARNESS_PH_BLIND_A),
                   harness_delta_arrivals(ps, HARNESS_PH_BLIND_A)),
               (unsigned long long)blind_pm,
               (unsigned long long)harness_permille(
                   harness_delta_shed(ps, HARNESS_PH_BLIND_C),
                   harness_delta_arrivals(ps, HARNESS_PH_BLIND_C)),
               (unsigned long long)harness_permille(
                   harness_delta_shed(ps, HARNESS_PH_RECOVERED),
                   harness_delta_arrivals(ps, HARNESS_PH_RECOVERED)),
               (unsigned long long)harness_delta_stale(ps, HARNESS_PH_LIVE),
               (unsigned long long)harness_delta_stale(ps, HARNESS_PH_BLIND_B),
               (unsigned long long)harness_delta_stale(ps,
                                                       HARNESS_PH_RECOVERED),
               (unsigned long long)harness_delta_admitted(ps,
                                                          HARNESS_PH_PRIME,
                                                          HARNESS_PH_END),
               (unsigned long long)harness_ceiling(ps, HARNESS_PH_PRIME,
                                                   HARNESS_PH_END));

        /* The route split for the two phases that assert a publications-derived
         * bound. A count that breaches that bound is only diagnosable with this
         * line: torn and future are bounded by publications and belong under
         * it, whereas aged is bounded by ARRIVALS and does not -- so an aged
         * term here says the bound is being applied to a regime its derivation
         * never covered, not that the seqlock is tearing more than expected. */
        printf("  HARNESS stale[%d] routes: live torn=%llu future=%llu"
               " aged=%llu | recovered torn=%llu future=%llu aged=%llu"
               " | bound live=%llu recovered=%llu\n",
               (int)i,
               (unsigned long long)harness_delta_torn(ps, HARNESS_PH_LIVE),
               (unsigned long long)harness_delta_future(ps, HARNESS_PH_LIVE),
               (unsigned long long)harness_delta_aged(ps, HARNESS_PH_LIVE),
               (unsigned long long)harness_delta_torn(ps,
                                                      HARNESS_PH_RECOVERED),
               (unsigned long long)harness_delta_future(ps,
                                                        HARNESS_PH_RECOVERED),
               (unsigned long long)harness_delta_aged(ps,
                                                      HARNESS_PH_RECOVERED),
               (unsigned long long)harness_publications_bound(
                   ps, HARNESS_PH_LIVE),
               (unsigned long long)harness_publications_bound(
                   ps, HARNESS_PH_RECOVERED));

        /* The same identity the gate enforces, restated per phase: a split
         * that did not sum to the total would make every route number above
         * unreadable. */
        ADCE_TEST_ASSERT(harness_delta_torn(ps, HARNESS_PH_RECOVERED) +
                         harness_delta_future(ps, HARNESS_PH_RECOVERED) +
                         harness_delta_aged(ps, HARNESS_PH_RECOVERED) ==
                         harness_delta_stale(ps, HARNESS_PH_RECOVERED));
        /* The tap ordering still holds under this scenario. Free to check, and
         * it rules out the deltas below being measured off a site that had
         * stopped accounting for its arrivals. */
        ADCE_TEST_ASSERT(site_identity_holds(&ps->site));
        total_tapped += ps->site.tapped;
    }

    for (i = 0; i < HARNESS_STALE_THREADS; ++i) {
        const harness_phased_t *ps = &g_st_sites[i];
        uint64_t live_pm;
        uint64_t blind_pm;
        int phase;

        live_pm = harness_permille(harness_delta_shed(ps, HARNESS_PH_LIVE),
                                   harness_delta_arrivals(ps, HARNESS_PH_LIVE));
        blind_pm =
            harness_permille(harness_delta_shed(ps, HARNESS_PH_BLIND_B),
                             harness_delta_arrivals(ps, HARNESS_PH_BLIND_B));

        ADCE_TEST_ASSERT(harness_check_live_phase(ps, HARNESS_PH_LIVE,
                                                  "live") == 0);

        for (phase = HARNESS_PH_BLIND_A; phase <= HARNESS_PH_BLIND_C; ++phase) {
            ADCE_TEST_ASSERT(harness_check_blind_phase(ps, phase,
                                                       "blind") == 0);
        }

        ADCE_TEST_ASSERT(harness_check_live_phase(ps, HARNESS_PH_RECOVERED,
                                                  "recovered") == 0);

        /* Requirement 4 over the ENTIRE window including the transition: from
         * the freeze, through the ageing window where reads flip from fresh to
         * stale, to the moment publication resumed. The transition is where a
         * ceiling that was only ever checked in steady state could hide a
         * burst. */
        ADCE_TEST_ASSERT(harness_delta_admitted(ps, HARNESS_PH_AGING,
                                                HARNESS_PH_RECOVERED) <=
                         harness_ceiling(ps, HARNESS_PH_AGING,
                                         HARNESS_PH_RECOVERED));

        /* And over the whole run, so no phase boundary can launder a burst. */
        ADCE_TEST_ASSERT(harness_delta_admitted(ps, HARNESS_PH_PRIME,
                                                HARNESS_PH_END) <=
                         harness_ceiling(ps, HARNESS_PH_PRIME,
                                         HARNESS_PH_END));

        /* The two postures are distinguishable, not merely each inside a band
         * that the other could also satisfy. */
        ADCE_TEST_ASSERT(blind_pm >= live_pm + HARNESS_BAND_SEPARATION);

    }

    /* The same overrun identity as test_harness_concurrent, with this
     * closer's own thaw drain folded into the discarded term -- the harness's
     * missed-epoch policy here is the shipped one, applied by hand (see the
     * comment on harness_closer_main), so the accounting has to match it
     * exactly. Every read below is race-free: both ingress threads and the
     * closer are already joined above, and g_st_obs.arrivals_closed /
     * g_st_thaw_discarded are each written by exactly one of those now-joined
     * threads. */
    residual = atomic_load_explicit(&g_st_counter.arrivals, memory_order_relaxed);
    ADCE_TEST_ASSERT(total_tapped ==
                     g_st_obs.arrivals_closed + g_st_thaw_discarded + residual);

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
ADCE_HARNESS_TEST_EXPORT(harness_stale_posture)
