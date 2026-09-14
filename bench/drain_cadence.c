/* Does draining T slots instead of one perturb the epoch cadence?
 *
 * PRE-REGISTERED. This file is committed with NO MEASURED NUMBERS IN IT,
 * before the measurement is run, and `git log` on this path is the proof of
 * order. Everything below is derivable from the design alone.
 *
 * =====================================================================
 * THE QUESTION
 * =====================================================================
 *
 * Before this change adce_obs_epoch_close ended one shared counter's epoch
 * with a single atomic exchange. It now calls adce_obs_drain, which performs
 * one exchange per CLAIMED slot. That work is off the arrival path -- it runs
 * on the observer thread, once per ADCE_OBS_EPOCH_NS -- but "off the hot path"
 * is not the same as "free", and the Observation Plane's cadence is the thing
 * every downstream staleness judgement is measured against. If closing an
 * epoch got slower in proportion to the ingress thread count, the plane would
 * have traded a per-arrival cost for a per-epoch one, and the trade would be
 * worth knowing about rather than assuming away.
 *
 * =====================================================================
 * THE DISCRIMINATING PREDICTION
 * =====================================================================
 *
 * NO PERTURBATION IS RESOLVABLE. The standing figure for the drain is 39 ns
 * for 64 uncontended exchanges, measured on the development host alongside
 * #33's contention sweep. ADCE_OBS_EPOCH_NS is 10 ms. That is a duty cycle of
 * about 4e-6, four orders of magnitude below the jitter of a sleeping thread's
 * wakeup on any host in this project's matrix, so the drain cannot move the
 * cadence by an amount this instrument can see.
 *
 * Stated so it can fail: the inter-publication interval at the maximum slot
 * count must not differ from the interval at one slot by more than the spread
 * observed WITHIN either arm. The within-arm spread is the yardstick precisely
 * because it is measured here rather than assumed -- a fixed threshold would
 * be a band with no derivation, which this project rules out.
 *
 * A SECOND, WEAKER BOUND IS NAMED because the 39 ns was measured on lines the
 * loop had just touched, and this drain touches them cold. Each slot is a full
 * ADCE_CACHELINE, so a full registry spans ADCE_OBS_MAX_INGRESS cache lines --
 * kilobytes -- pulled in once per epoch after 10 ms of doing nothing else.
 * Charging every one of them a cold miss still leaves the drain below a
 * thousandth of the period. So the prediction survives both mechanisms, and
 * the two are worth separating anyway: cold misses scale with the slot count
 * the same way the exchanges do, so a result that scales STEEPER than linearly
 * in the slot count refutes both readings at once and says the cost is
 * somewhere this analysis has not looked.
 *
 * WHAT WOULD REFUTE IT. Any of: the interval's median rising with the slot
 * count beyond the within-arm spread; the late-epoch count rising with the
 * slot count; the tail of the interval distribution widening with the slot
 * count. Each of those is reported per arm below, so the refutation is
 * readable off the output rather than argued for afterwards.
 *
 * =====================================================================
 * THE PROTOCOL
 * =====================================================================
 *
 *  - One real adce_obs_thread per arm, at the shipped ADCE_OBS_EPOCH_NS. The
 *    scheduling half is the subject, so it is not simulated.
 *
 *  - The arms are SLOT COUNTS: 1, then a sweep up to ADCE_OBS_MAX_INGRESS. The
 *    slots are claimed and left IDLE -- no thread taps them. That is
 *    deliberate and it is the worst case rather than an omission: a tapped
 *    slot's line is resident somewhere, and an idle one has had 10 ms to be
 *    evicted, so an idle registry maximises the per-slot cost the drain pays.
 *    It also isolates the drain, which is the question, from ingress
 *    contention, which #33 already answered.
 *
 *  - CADENCE IS READ FROM THE PUBLICATION, not from the sampler. A poller
 *    watches epoch_id through adce_epoch_read and, on each advance, records
 *    the observed_at_ns the observer itself stamped at close. Inter-publication
 *    intervals are differences of those stamps, so the poller's own wakeup
 *    jitter cannot enter the measurement -- it can only cost a sample, and a
 *    missed advance is counted and reported rather than interpolated.
 *
 *  - MEDIAN, with min, p99 and max beside it. Never the mean: a descheduled
 *    observer is a one-sided outlier describing the host, and this project
 *    has an entry about letting one of those into an average.
 *
 *  - late_epochs and skipped_epochs are read from the observer after the join,
 *    per arm, because they are the plane's own opinion of whether it kept
 *    cadence and they cost nothing to report.
 *
 *  - Every figure carries its n. Runs are repeated and each arm reports the
 *    number of intervals it is summarising.
 *
 * ASSERTS NOTHING, for the same reason bench/tap_contention.c asserts nothing:
 * a threshold on a host's scheduling would be a band with no derivation
 * evaluated on a shared runner. It is a benchmark, not a gate.
 *
 * Build (the strict profile's flags; it links the producer and the observer):
 *   cc -std=c11 -O2 -Wall -Wextra -Werror -Wconversion -Wshadow -Wcast-align \
 *      -Wstrict-prototypes -Wpointer-arith -Wvla -pedantic -Iinclude \
 *      -o drain_cadence bench/drain_cadence.c src/adce_observe.c \
 *      src/adce_obs_thread.c -lpthread -lm
 *
 * Usage: drain_cadence [epochs_per_arm] [runs]
 */

#include "adce_obs_thread.h"
#include "adce_observe.h"
#include "adce_platform.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAD_MAX_EPOCHS 4096

static int cad_cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;

    if (x < y) {
        return -1;
    }
    return x > y ? 1 : 0;
}

/* Order statistic on an already-sorted array. Interpolation would invent
 * precision the sample size does not carry. */
static uint64_t cad_pct(const uint64_t *sorted, unsigned n, unsigned pct) {
    unsigned idx;

    if (n == 0u) {
        return 0;
    }
    idx = (unsigned)(((uint64_t)pct * (uint64_t)(n - 1u)) / 100u);
    return sorted[idx];
}

typedef struct {
    uint64_t interval[CAD_MAX_EPOCHS];
    unsigned n;
    unsigned missed_advances;
    uint64_t late_epochs;
    uint64_t skipped_epochs;
} cad_result_t;

/* One arm: start an observer, claim `slots` counters, watch the publication
 * stream until `want` intervals have been collected or the budget expires. */
static int cad_run_arm(unsigned slots, unsigned want, cad_result_t *out) {
    static adce_obs_thread_t obs;
    static adce_epoch_state_t epoch;
    uint64_t last_stamp = 0;
    uint64_t deadline;
    uint64_t last_id = 0;
    unsigned claimed = 0;
    unsigned i;
    int have_last = 0;

    memset(out, 0, sizeof(*out));
    memset(&epoch, 0, sizeof(epoch));

    if (!adce_obs_thread_start(&obs, &epoch)) {
        return 0;
    }

    for (i = 0; i < slots; ++i) {
        if (adce_obs_claim_counter(&obs.ctx) != NULL) {
            claimed++;
        }
    }
    if (claimed != slots) {
        adce_obs_thread_stop(&obs);
        return 0;
    }

    /* Budget generously: the arm ends on sample count, and the deadline exists
     * only so a stalled observer cannot hang the benchmark. */
    deadline = adce_now_ns() +
               (uint64_t)(want + 8u) * ADCE_OBS_EPOCH_NS * 4ULL;

    while (out->n < want && adce_now_ns() < deadline) {
        adce_q16_t pressure;
        uint64_t id;
        uint64_t stamp;

        if (adce_epoch_read(&epoch, &pressure, &id, &stamp) && id != last_id) {
            if (have_last) {
                /* An id jumping by more than one means the poller missed a
                 * publication; the interval spans two epochs and is not
                 * comparable, so it is counted and dropped. */
                if (id == last_id + 1u) {
                    out->interval[out->n++] = stamp - last_stamp;
                } else {
                    out->missed_advances++;
                }
            }
            last_id = id;
            last_stamp = stamp;
            have_last = 1;
        }
        ADCE_CPU_RELAX();
    }

    adce_obs_thread_stop(&obs);
    out->late_epochs = obs.late_epochs;
    out->skipped_epochs = obs.skipped_epochs;
    return 1;
}

int main(int argc, char **argv) {
    const unsigned arms[] = {1u, 4u, 16u, 64u};
    unsigned want = 200u;
    unsigned runs = 3u;
    unsigned a;
    unsigned r;

    if (argc > 1) {
        want = (unsigned)strtoul(argv[1], NULL, 10);
    }
    if (argc > 2) {
        runs = (unsigned)strtoul(argv[2], NULL, 10);
    }
    if (want == 0u || want > CAD_MAX_EPOCHS) {
        want = 200u;
    }

    printf("drain cadence: epoch=%llu ns, max_ingress=%d, cacheline=%d, "
           "ctx=%zu bytes\n",
           (unsigned long long)ADCE_OBS_EPOCH_NS, (int)ADCE_OBS_MAX_INGRESS,
           (int)ADCE_CACHELINE, sizeof(adce_obs_ctx_t));
    printf("intervals per arm per run: %u; runs: %u\n\n", want, runs);
    printf("%6s %5s %5s %12s %12s %12s %12s %6s %6s %7s\n", "slots", "run", "n",
           "median_ns", "min_ns", "p99_ns", "max_ns", "late", "skip", "missed");

    for (a = 0; a < sizeof(arms) / sizeof(arms[0]); ++a) {
        if (arms[a] > (unsigned)ADCE_OBS_MAX_INGRESS) {
            continue;
        }
        for (r = 0; r < runs; ++r) {
            static cad_result_t res;

            if (!cad_run_arm(arms[a], want, &res)) {
                printf("%6u %5u  ARM FAILED (start or claim refused)\n",
                       arms[a], r);
                continue;
            }
            qsort(res.interval, res.n, sizeof(res.interval[0]), cad_cmp_u64);
            printf("%6u %5u %5u %12" PRIu64 " %12" PRIu64 " %12" PRIu64
                   " %12" PRIu64 " %6" PRIu64 " %6" PRIu64 " %7u\n",
                   arms[a], r, res.n, cad_pct(res.interval, res.n, 50u),
                   cad_pct(res.interval, res.n, 0u),
                   cad_pct(res.interval, res.n, 99u),
                   cad_pct(res.interval, res.n, 100u), res.late_epochs,
                   res.skipped_epochs, res.missed_advances);
            fflush(stdout);
        }
    }

    printf("\nREAD THE PREDICTION AT THE TOP OF THIS FILE BEFORE THE TABLE.\n"
           "It is refuted if the median rises with the slot count by more than\n"
           "the spread across runs within an arm, if `late` rises with the slot\n"
           "count, or if the p99/max tail widens with it.\n");
    return 0;
}
