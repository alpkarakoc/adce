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
#include <stdio.h>

int main(void) {
    printf("pre-registration only; no measurement code yet\n");
    return 0;
}
