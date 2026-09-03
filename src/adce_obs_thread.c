/* Observer thread: the scheduling half of the Observation Plane.
 *
 * Everything here is cadence and lifecycle. Not one statistical decision is
 * made in this file -- the loop below reads the clock, decides whether this
 * wakeup is on time, and calls adce_obs_epoch_close(). The plane's arithmetic
 * stays in src/adce_observe.c, and observed_at_ns stays a parameter, so every
 * property the unit tests assert is still asserted without this thread ever
 * being started. */

#include "adce_obs_thread.h"

/* Sleeps at most one tick, and never past the deadline. Sleeping a full tick
 * unconditionally would overshoot every deadline that falls mid-tick and make
 * every epoch late by up to a tick; sleeping the whole remaining interval in
 * one call would make a stop request wait up to a full T. The minimum of the
 * two is what gives bounded stop latency AND an on-time epoch.
 *
 * EINTR is not retried. A short sleep costs one extra pass around the loop,
 * which re-reads the clock and recomputes the remaining interval from scratch,
 * so a partial sleep is absorbed by the next iteration rather than by an
 * accumulating error. */
static void obs_sleep_until(uint64_t now_ns, uint64_t deadline_ns) {
    struct timespec req;
    uint64_t remaining;

    if (now_ns >= deadline_ns) {
        return;
    }

    remaining = deadline_ns - now_ns;
    if (remaining > ADCE_OBS_THREAD_TICK_NS) {
        remaining = ADCE_OBS_THREAD_TICK_NS;
    }

    /* remaining is bounded by ADCE_OBS_THREAD_TICK_NS (1e6) above, so it fits
     * in tv_nsec on every target and the narrowing cast is exact. */
    req.tv_sec = 0;
    req.tv_nsec = (long)remaining;
    (void)nanosleep(&req, NULL);
}

/* One wakeup's worth of decision. Returns the next deadline.
 *
 * Three cases, and the third is the one that is usually assumed away:
 *
 *  - ON TIME. Overrun within ADCE_OBS_THREAD_LATE_NS. Close the epoch and
 *    advance the deadline by exactly T, so the schedule stays anchored to its
 *    original phase instead of drifting by the wakeup error each time.
 *
 *  - LATE, but inside one epoch. Counted, and closed anyway. The interval fed
 *    to the statistics is then up to 1.1T rather than T, which overstates the
 *    rate by at most 10% -- inside the noise the sigma floor already absorbs,
 *    and in the conservative direction (a higher rate reads as higher pressure,
 *    which sheds more, not less).
 *
 *  - MISSED. The wakeup landed a full epoch or more past its deadline, so the
 *    counter holds the arrivals of several epochs. Those arrivals are DROPPED,
 *    not closed. Closing them as one epoch would divide a multi-epoch total by
 *    a single T and manufacture a rate spike out of a scheduler artifact --
 *    which at a large enough overrun saturates the squash and sheds
 *    everything, turning a descheduled observer into an outage. That is
 *    precisely the failure docs/enforcement-plane.md section 4 refuses. Nor
 *    can the loop catch up by closing several epochs in a row: only the first
 *    would find any arrivals and the rest would report zero, so a stall would
 *    read as a spike followed by a collapse. Neither is true.
 *
 *    Discarding instead leaves the statistics untouched and, more importantly,
 *    leaves observed_at_ns where it was. The epoch therefore ages toward
 *    ADCE_ADVICE_TIMEOUT_NS on its own and adce_epoch_is_stale() takes
 *    Enforcement to its conservative posture with no second mechanism -- the
 *    same watchdog that already covers a dead observer now also covers a
 *    merely slow one. The discard is recorded rather than silent, because a
 *    thread quietly throwing arrivals away is otherwise indistinguishable from
 *    one that is working. */
static uint64_t obs_service_deadline(adce_obs_thread_t *t, uint64_t now_ns,
                                     uint64_t deadline_ns) {
    uint64_t overrun;
    int rc;

    if (now_ns < deadline_ns) {
        return deadline_ns;
    }

    overrun = now_ns - deadline_ns;

    if (overrun >= ADCE_OBS_EPOCH_NS) {
        t->skipped_epochs += overrun / ADCE_OBS_EPOCH_NS;
        t->discarded_arrivals += adce_obs_counter_take(t->ctx.counter);
        /* Re-anchored to now rather than advanced by T: the old phase is gone
         * and stepping toward it one epoch per pass would spin through the
         * whole backlog without sleeping. */
        return now_ns + ADCE_OBS_EPOCH_NS;
    }

    if (overrun > ADCE_OBS_THREAD_LATE_NS) {
        t->late_epochs++;
    }

    rc = adce_obs_epoch_close(&t->ctx, now_ns);

    /* -1 is "not the claimed writer", which this thread cannot be: it won the
     * claim before entering the loop and the claim is never released. Counted
     * rather than ignored so the epoch accounting stays total -- an epoch that
     * neither published nor warmed has to appear somewhere. */
    if (rc < 0) {
        t->skipped_epochs++;
    }

    /* overrun < T, so this is strictly greater than now_ns and the loop always
     * sleeps before the next service. */
    return deadline_ns + ADCE_OBS_EPOCH_NS;
}

static void *obs_thread_main(void *arg) {
    adce_obs_thread_t *t = (adce_obs_thread_t *)arg;
    uint64_t deadline_ns;

    /* Claimed HERE, on the observer thread, not in adce_obs_thread_start().
     * adce_obs_claim_writer records pthread_self() as the owner, and the
     * ownership test in adce_obs_epoch_close compares against the CALLING
     * thread -- so a claim made by the starting thread would record the wrong
     * owner and every close would return -1 and publish nothing, silently. */
    if (!adce_obs_claim_writer(&t->ctx)) {
        atomic_store_explicit(&t->ready, -1, memory_order_release);
        return NULL;
    }

    deadline_ns = adce_now_ns() + ADCE_OBS_EPOCH_NS;

    /* Release: pairs with the acquire in adce_obs_thread_start, and publishes
     * the completed claim -- ctx.owner and ctx.owner_valid included -- to the
     * starting thread before it is allowed to return. */
    atomic_store_explicit(&t->ready, 1, memory_order_release);

    while (atomic_load_explicit(&t->running, memory_order_acquire)) {
        uint64_t now_ns = adce_now_ns();

        deadline_ns = obs_service_deadline(t, now_ns, deadline_ns);
        obs_sleep_until(adce_now_ns(), deadline_ns);
    }

    return NULL;
}

int adce_obs_thread_start(adce_obs_thread_t *t, adce_obs_counter_t *counter,
                          adce_epoch_state_t *epoch) {
    int ready;

    memset(t, 0, sizeof(*t));
    adce_obs_init(&t->ctx, counter, epoch);

    atomic_store_explicit(&t->running, 1, memory_order_relaxed);
    atomic_store_explicit(&t->ready, 0, memory_order_release);

    if (pthread_create(&t->thread, NULL, obs_thread_main, t) != 0) {
        atomic_store_explicit(&t->running, 0, memory_order_relaxed);
        return 0;
    }
    t->started = 1;

    /* Start is synchronous by design. Returning before the claim resolved
     * would let a caller begin ingress against a context whose writer claim is
     * about to be REFUSED -- an observer that publishes nothing while looking
     * started, which is the failure mode hardest to notice, because the
     * watchdog quietly covers for it forever. */
    while ((ready = atomic_load_explicit(&t->ready, memory_order_acquire)) == 0) {
        obs_sleep_until(0, ADCE_OBS_THREAD_TICK_NS);
    }

    if (ready < 0) {
        adce_obs_thread_stop(t);
        return 0;
    }

    return 1;
}

void adce_obs_thread_stop(adce_obs_thread_t *t) {
    if (!t->started) {
        return;
    }

    /* Cooperative flag plus join, and pthread_cancel is deliberately not used.
     * A cancellation landing between the two sequence increments of
     * adce_epoch_publish would leave the sequence ODD forever: every
     * subsequent adce_epoch_read returns 0, every reader treats that as stale,
     * and the whole system holds the conservative posture permanently with no
     * way back short of a restart. Fail-closed, but unrecoverable, and caused
     * by the shutdown rather than by any fault.
     *
     * The flag is only ever read at the TOP of the loop, so a publication
     * already in flight is never interrupted: the thread finishes
     * adce_epoch_publish, returns to the loop head, observes the flag and
     * exits. There is no window in which a stop can tear a publication.
     *
     * The join is what makes the thread's writes -- ctx statistics and the
     * scheduling diagnostics -- readable by the caller without a race. */
    atomic_store_explicit(&t->running, 0, memory_order_release);
    (void)pthread_join(t->thread, NULL);

    /* Idempotent: a second stop finds started == 0 and returns. Joining an
     * already-joined pthread_t is undefined, so this flag is the guard, not a
     * convenience. */
    t->started = 0;
}
