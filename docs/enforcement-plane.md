# Enforcement Plane — design

Status: proposed. No implementation exists. Written before any consumer code so the
semantics read out of `adce_epoch_state_t` are pinned first, the same way
`docs/observation-plane.md` pinned what is written into it.

The Observation Plane publishes and nothing consumes. This document decides what consuming
means.

## Constraints found in the repository at time of writing

Three are locked and this design is written inside them, not around them.

- **The RNG lane is already decided.** `adce_platform.h` says of `adce_rng_next_unit()`:
  *"Confined to the Observation Plane by convention: the Ingest/Enforcement planes must
  drive stochastic drop decisions from `adce_rng_next()` and integer comparisons only."*
  So the drop decision is integer arithmetic on a raw 64-bit draw. No double appears
  anywhere in this plane. This is not a limitation to work around — it is what makes the
  decision exactly reproducible from a recorded draw, which section 5 depends on.
- **`adce_rng_tls` is one stream per translation unit, per thread.** It is declared
  `static _Thread_local` at file scope in a header, so every TU that includes the header
  gets its own copy, each seeded independently from the kernel. Verified by taking its
  address from two TUs in one binary: different objects. A test therefore *cannot* observe
  or seed the stream the enforcement TU draws from. This is the single most consequential
  fact for the shape of the API below.
- **`adce_rng_seed` calls `abort()` on entropy failure**, and `adce_rng_next()` seeds
  lazily on first use. So the first gate call on a fresh ingress thread can abort. That is
  a locked decision, and section 1.4 says what it means for the caller rather than
  softening it.

Nothing here contradicts CLAUDE.md or the observation design. Where this document makes a
choice the observation design left open, it says so.

## 1. The decision function

Two stages, in this order, per arrival:

    pressure ──▶ [1. STOCHASTIC SHED]  drop with probability p
                        │ survived
                        ▼
                 [2. TOKEN BUCKET]     drop if no token available
                        │ survived
                        ▼
                     ADMIT

Neither stage alone is sufficient, and the reason is different in each direction.

### 1.1 Stage one: proportional stochastic shedding

    admit_unless(draw >> 48 < pressure_q16)

`pressure_q16` is the re-clamped value from the epoch read, in `[0, ADCE_PRESSURE_MAX]`
where `ADCE_PRESSURE_MAX == ADCE_Q16_ONE == 65536`. `draw` is one `adce_rng_next()` word.

The shift is the whole mapping and it is exact. `draw >> 48` partitions the 64-bit output
space into 65536 buckets of exactly 2^48 values each, so the drop fraction is `p / 65536`
with no modulo bias at any `p`. Verified across the range: `p=0` drops 0/65536, `p=16384`
drops exactly 16384/65536, `p=65536` drops 65536/65536. The two endpoints are the ones
that matter most — `pressure == 0` admits everything and `pressure == ADCE_PRESSURE_MAX`
drops everything — and both are exact rather than off by one bucket.

**Why the top 16 bits and not the low ones.** xorshift128+ is weakest in its low bits; its
lowest bit is a pure LFSR sequence and fails linear-complexity tests. The high bits carry
the addition's carry propagation and are the ones the generator's authors intend for use.
`adce_rng_next_unit()` already takes the top 53 for the same reason. Taking `draw & 0xFFFF`
instead would bias the containment decision in a way no test of the *fraction* would
detect, because the fraction would still be right — only its independence across draws
would be wrong.

**Failure mode this stage prevents: the threshold cliff.** A gate that admits below a
pressure threshold and drops above it converts a smooth pressure signal into a binary
actuator. Traffic sitting near the threshold then oscillates — admit fully, pressure rises,
drop fully, pressure falls — which is the same limit cycle `observation-plane.md` §2 warns
about, recreated by the actuator instead of by a misplaced tap. Proportional shedding has
no cliff: a pressure of 0.3 sheds 30%, and the response is continuous in the signal.

### 1.2 Stage two: the token bucket, and what `adce_token_*` is for

`adce_token_refill` and `adce_token_try_take` already exist and are tested at the
arithmetic level. This is what they are for; nothing else in the codebase uses them.

    tokens = adce_token_refill(tokens, ADCE_ENF_RATE_Q16_PER_NS,
                               now_ns - last_refill_ns, ADCE_ENF_CAPACITY_Q16);
    last_refill_ns = now_ns;
    admit = adce_token_try_take(&tokens, ADCE_ENF_COST_Q16);

Pressure does **not** map onto rate and capacity. The bucket's rate and capacity are fixed
configuration, independent of pressure, and that independence is the point.

**Failure mode this stage prevents: the detector adapting to the attack.** `pressure` is a
z-score — a *relative* measure of deviation from the EWMA of the recent past, with a ~1 s
effective window at T = 10 ms and N = 100. An attacker who ramps load slowly raises `mu`
along with the rate, so `d` stays small, `z` stays below `z_lo`, and pressure stays at zero
while absolute volume climbs without bound. The Observation Plane is not defective here; it
was never designed to bound absolute volume, and §3 of its design says so. The token bucket
is the ceiling that does not require the detector to agree that anything is wrong. It is
the floor of protection under a detector that has been talked out of alarming.

Making rate a function of pressure would destroy exactly this property: at pressure zero
the bucket would open fully, and the slow ramp would pass unimpeded. The two stages must
fail independently or they are one stage.

**Why this order.** Shedding is stateless and touches no shared line; the bucket has
mutable state. Shedding first means requests that are going to be dropped anyway never
touch bucket state, which keeps the write traffic on that state proportional to *admitted*
load rather than offered load. Bucket-first would also consume tokens for requests
subsequently shed at random, making the admitted rate silently lower than the configured
ceiling.

### 1.3 Is per-thread xorshift128+ sufficient?

For uniformity and independence, yes: period 2^128−1, each thread's state seeded
independently from the kernel CSPRNG, no shared state, no lock, one register-only step per
call. It meets the hot-path rule that banned `rand()`.

For unpredictability against an adversary, **not unconditionally**, and this is stated
rather than assumed. xorshift128+ is linear over GF(2) and is not cryptographically secure;
given enough raw outputs its state is recoverable and all future outputs follow. The gate
does not emit raw outputs — an observer sees one bit per request, the comparison result,
not the word. Recovering 128 bits of state from single-bit observations at an unknown and
time-varying `pressure` is far harder than from raw output, and no practical attack on this
observation model is known to me.

The condition under which it would matter is specific and worth writing down: an adversary
who can drive a large number of requests, observe each outcome, and hold `pressure` fixed
and known while doing so. Such an adversary could in principle predict which future
requests will be shed and time an attack into the admitted gaps. The mitigation is a
different generator, which is a separate decision with its own cost, and it is listed as an
open decision rather than defaulted here.

### 1.4 The abort on first use

`adce_rng_next()` seeds lazily and `adce_rng_seed()` aborts if the entropy draw fails.
Composed with a gate that calls it per arrival, the consequence is that **the first
arrival on a new ingress thread can abort the process**, mid-traffic, at an arbitrary
moment.

The seeding decision is locked and correct — a predictable PRNG makes every containment
decision predictable — so the fix is not to soften it but to move *when* it fires. The
plane should expose a `adce_enf_thread_init()` that draws once at thread startup. Then an
entropy failure aborts during thread creation, where a supervisor can observe it, instead
of on a request path. Calling it is the caller's responsibility and its absence is not
detectable by the gate, so this belongs in the ingress recipe in §3.

### 1.5 What the gate returns

Three outcomes, not two: `ADMIT`, `DROP_SHED`, `DROP_LIMIT`. Distinguishing the two drops
costs nothing and tells an operator whether containment or the absolute ceiling is doing
the work — which are different problems with different responses.

This is the telemetry `observation-plane.md` §2 permits, and its final sentence binds here:
these outcomes must never feed the pressure statistic. Counting them is fine. Routing them
back into the arrival count, or into `mu`, closes the actuation loop by another route and
recreates precisely the self-soothing failure the tap placement exists to prevent.

## 2. Where the epoch state is read, and how often

**Per arrival, and the clock read is the reason it is affordable.**

The obvious objection is that this puts a seqlock read on the hot path. The cost is real
but small: `adce_epoch_read` is an acquire load, three relaxed loads, an acquire fence, and
a second acquire load, all from one cache line that is written once per 10 ms and read
constantly — so it sits in every core's L1 in shared state, and the writer invalidates it
100 times a second. On x86_64 the acquire loads are plain `MOV` and the fence is a no-op.
On arm64 they are `LDAR` and the fence is a `DMB ISHLD`, which is the more expensive case
and the one to measure. The retry loop is unbounded in principle; in practice a single
writer publishing once per 10 ms makes the odds of landing inside a write on the order of
the write's duration divided by 10 ms.

The decisive point is that **the gate needs `now_ns` regardless**, because
`adce_token_refill` takes `elapsed_ns` and because staleness is a function of the current
time. On Linux that is a vDSO `clock_gettime`, roughly 20–25 ns — very likely more
expensive than the seqlock read it would be avoided in favour of. Caching the snapshot to
save the seqlock read while still reading the clock every arrival optimises the cheaper
half.

**Is a cached read, refreshed once per epoch, safe?** The snapshot may be cached. The
staleness verdict may not. Staleness is `(now - observed_at_ns) > ADCE_ADVICE_TIMEOUT_NS`,
a function of `now` and not of the snapshot alone, so it has to be recomputed on every
arrival from a fresh clock read even if `pressure` and `observed_at_ns` come from a cached
copy. A design that caches the verdict alongside the payload is the dangerous one: it
freezes a decision that exists precisely to change over time.

**What a reader that never refreshes does** — and this is the reassuring part. It holds one
snapshot forever, recomputes staleness against a fresh `now` each arrival, and therefore
goes stale exactly `ADCE_ADVICE_TIMEOUT_NS` after that snapshot's `observed_at_ns` and
stays there. It falls into the conservative posture of §4 and never leaves. It does not
fail open, and it does not silently keep acting on a snapshot from an hour ago. The
degradation is correct by construction, which is the property that makes the caching
question a performance question rather than a safety one.

Recommendation: read per arrival, measure, and only then consider caching. The cache is an
optimisation with a real correctness trap attached, and nothing yet shows it is needed.

**Measured, and the prediction held.** `test/t_adce_latency.c` now reports both halves on
every run. On an Apple M3 (arm64, so `LDAR` and a real `DMB ISHLD`, the expensive case
named above), strict `-O2`: the whole gate costs **~2.9–4.3 ns** depending on outcome,
while `adce_now_ns` alone costs **~13 ns**. The clock read is therefore roughly three to
four times the entire gate — seqlock read, clamp, shed test, refill and take together —
and the paragraph above understated the gap rather than overstating it.

That settles the caching question in the direction the argument predicted, and more
strongly. Caching the snapshot to avoid the seqlock read would remove at most ~4 ns from a
~17 ns path while the clock read it cannot avoid keeps the other ~13 ns, and it would buy
that by taking on the correctness trap in the next paragraph. **Do not cache.** The
measurement is what turns that from a preference into a finding.

## 3. Call ordering at an ingress site

This is the project's core principle and it has not previously been written as an
executable sequence. This is what a caller actually writes:

```c
#include "adce_observe.h"
#include "adce_enforce.h"

static adce_obs_counter_t g_arrivals;
static adce_enf_ctx_t     g_enforce;   /* per-thread; see the note below */

/* Once per ingress thread, at thread start -- not on the request path.
 * Draws the RNG seed here so an entropy failure aborts during startup,
 * where a supervisor sees it, rather than mid-traffic (section 1.4). */
static void ingress_thread_start(void) {
    adce_enf_thread_init(&g_enforce, &g_epoch_state, adce_now_ns());
}

static int ingress_handle(struct conn *c) {
    uint64_t now_ns;
    adce_enf_outcome_t outcome;

    /* 1. TAP. Unconditional, and first. Every arrival is counted, including
     *    the ones the gate is about to drop. Nothing may return before this
     *    line -- not a parse failure, not a rate-limit shortcut, not an
     *    early exit for malformed input. An arrival the tap does not see is
     *    an arrival the detector believes did not happen. */
    adce_obs_tap(&g_arrivals);

    /* 2. GATE. One clock read serves both the staleness check and the
     *    bucket refill (section 2). */
    now_ns = adce_now_ns();
    outcome = adce_enf_admit(&g_enforce, now_ns);

    if (outcome != ADCE_ENF_ADMIT) {
        /* Counting drops is fine. Feeding them back into the arrival
         * counter or into the EWMA is not (section 1.5). */
        return conn_reject(c, outcome);
    }

    /* 3. WORK. Only reached by admitted arrivals. */
    return conn_serve(c);
}
```

The property is structural, not behavioural: the tap is not inside the gate, not after it,
not conditional on it, and not skippable by any earlier return. Every one of those is a
way to get it wrong that still compiles and still passes every unit test in the repository.

The inverted version, for contrast — this is the shape to reject in review:

```c
    /* WRONG. The detector now measures what enforcement let through, so
     * containment suppresses its own input signal: drops reduce the
     * measured rate, pressure falls, containment disengages, and the
     * attacker is admitted precisely because the attack was working. */
    if (adce_enf_admit(&g_enforce, adce_now_ns()) != ADCE_ENF_ADMIT) {
        return conn_reject(c, ADCE_ENF_DROP_SHED);
    }
    adce_obs_tap(&g_arrivals);
    return conn_serve(c);
```

**On `adce_enf_ctx_t` being per-thread.** The bucket carries mutable state — `tokens` and
`last_refill_ns` — and mutexes and spinlocks are banned on this path. A shared bucket would
need a CAS loop on a contended line touched by every arrival on every thread, which is the
false sharing the rest of the codebase pads against. Per-thread contexts have no
contention, and they compose with the RNG, which is already per-thread per-TU.

The cost is explicit and must not be discovered later: with per-thread buckets the
aggregate admitted ceiling is `threads × ADCE_ENF_RATE`, not `ADCE_ENF_RATE`. The constant
should be named for the per-thread quantity it is, so nobody reads it as a global ceiling.

This gives the plane a clean concurrency contract: **the epoch state is the only shared
object, and Enforcement only ever reads it. Everything Enforcement writes is per-thread.**

## 4. The conservative posture on stale

**A fixed fallback pressure, and specifically not maximal.**

Both extremes fail, differently:

- **Drop everything.** The watchdog trips when the observer thread dies, is descheduled, or
  falls five epochs behind. That is a fault in the *detector*, not evidence of an attack.
  Converting a monitoring failure into a total outage makes the system strictly less
  available than having no detector at all — which is a sound argument for deleting the
  detector, and therefore an argument this design must not hand anyone. A GC pause or
  scheduler starvation lasting 50 ms would blackhole all traffic.
- **Admit everything.** This is the precise failure the watchdog exists to prevent. It also
  makes the observer thread a single point of attack: anyone who can stall or kill it turns
  containment off completely, and the cheapest attack on the system becomes an attack on
  its monitoring.

So: `ADCE_ENF_STALE_PRESSURE`, a compile-time constant substituted for the published
pressure whenever `adce_epoch_is_stale` is true. Proposed value `ADCE_Q16_ONE / 2` — shed
half.

**What makes a non-maximal fallback safe is stage two.** The token bucket does not depend
on the epoch state at all, so it keeps enforcing its absolute ceiling throughout the stale
window. The fallback pressure bounds the *fraction* admitted; the bucket bounds the
*absolute rate*. Neither the detector's health nor its opinion is required for the second
one to hold.

**The cold-start interaction decides this, and it is the strongest argument.** A
zero-initialised `adce_epoch_state_t` has `observed_at_ns == 0`, so it reads stale from the
first instant — that is the entire cold-start mechanism `observation-plane.md` §1 relies
on, and it stays untouched. But warmup suppresses publication for `ADCE_OBS_WARMUP_EPOCHS`
= 100 epochs = **one full second at every process start**. A maximal fallback would
therefore mean a one-second total outage on every deploy, restart, and scale-out event.
Half pressure plus the bucket means a startup that sheds heavily and recovers, which is a
posture rather than an outage.

Note this makes `ADCE_ENF_STALE_PRESSURE` do double duty: it is both the observer-failure
posture and the startup posture. Those could reasonably want different values. Listed as an
open decision rather than defaulted.

**Re-clamping at read** is unconditional and comes first, before the staleness branch. The
published value is re-clamped through `adce_obs_pressure_clamp` on the way out of
`adce_epoch_read`, so a negative reads as maximal and an out-of-range positive reads as
maximal, per the locked rule. A reader that trusts the writer's clamp has no defence
against a torn or corrupted read, and the read side is where the plane boundary is actually
crossed.

### 4.1 The three routes into the stale verdict

Crossing `ADCE_ADVICE_TIMEOUT_NS` is **not** the only way the gate reaches its conservative
posture, and the other two are not faults. This was written after the integration harness
measured them; before that, this section read as though ageing were the only route, and a
reader seeing nonzero `stale_reads` on a healthy system had nothing to check it against.

1. **Aged.** `adce_epoch_is_stale` is true because the last publication really is older than
   the timeout. The observer is dead, descheduled, or behind. This is the route this section
   was written for.
2. **Torn.** `adce_epoch_read` returned 0 — the reader landed inside the writer's sequence
   window — and `adce_enf_decide` takes the stale branch without consulting
   `adce_epoch_is_stale` at all. No snapshot means no advice, which is the conservative
   reading; retrying on the arrival path would be unbounded work for a value that is about
   to be replaced anyway.
3. **Future.** A publication landed between the caller's `adce_now_ns()` and the gate's
   epoch read, so `observed_at_ns` exceeds the `now_ns` already captured. The subtraction in
   `adce_epoch_is_stale` is unsigned, wraps to near `UINT64_MAX`, and the epoch reads as
   maximally stale. This one was accidental and is now a contract, stated at the function
   itself: **a future timestamp reads as maximally stale.** It is fail-closed in every case,
   and changing that arithmetic — a signed difference, a saturating guard, an explicit
   `observed_at_ns > now_ns` branch returning 0 — turns it fail-*open* exactly where the
   reader cannot order the publication against its own clock.

**Both 2 and 3 require a concurrent publication.** A sequential reader can straddle at most
one publication at a time, so their combined count is bounded by *publications in the
window*, never by arrivals. It does not grow with offered load. That is what distinguishes
them from the posture genuinely engaging, and it is the bound the harness asserts rather
than asserting zero.

**Measured**, over the harness's live phase — publication healthy throughout, four ingress
threads:

| | |
| --- | --- |
| Arrivals | ~238,000 per thread, ~950,000 total |
| Publications in the window | 27–28 |
| Stale reads | 2–4 total, across all four threads |
| Classified `aged` | **0**, in every instrumented run, max observed age 0 |
| Classified `torn` | 2–4 — all of them |
| Classified `future` | 0 in the live phase over ten runs; 1 across six whole runs |

Close-to-close publication cadence peaked at 11.2 ms against the 50 ms timeout, so
publication never came close to lapsing. **A nonzero `stale_reads` on a healthy system is
therefore expected**, at roughly one per 300,000 arrivals here, and it is not evidence of an
observer problem. What would be evidence is a count that scales with offered load rather
than with publication count.

## 5. Testable now vs. needs a harness

The boundary is drawn by one question: *does the property live inside a function, or in the
relationship between call sites?* Everything in the first category is a unit test.

**Testable without a running system**, all pure and deterministic — provided the decision
function takes the draw as a parameter:

- The shed mapping, exhaustively. `adce_enf_should_shed(p, draw)` over all 65536 top-16
  bucket values at fixed `p` gives an exact count, not a statistical estimate. Assert
  `p=0` sheds none, `p=ADCE_PRESSURE_MAX` sheds all, and every intermediate `p` sheds
  exactly `p` of 65536.
- Monotonicity: the set of shedding draws grows with `p`, and never shrinks.
- Re-clamp at read: negative and out-of-range positive both read as maximal.
- Stale substitution: an epoch older than `ADCE_ADVICE_TIMEOUT_NS` yields exactly
  `ADCE_ENF_STALE_PRESSURE`, and one at the boundary does not.
- Cold start: a zero-initialised epoch state takes the stale path on the very first call.
- Bucket integration: refill and take compose correctly over an injected `now_ns`
  sequence, and a bucket that has never been refilled does not admit on its first call.
- Determinism: a recorded sequence of `(pressure, draw, now_ns)` yields a bit-identical
  outcome sequence across runs.

**Requires the integration harness**, proposed as its own step:

- **That the tap precedes the gate.** This is a property of the call site and no unit test
  reaches it, because no function is wrong — `adce_obs_tap` and `adce_enf_admit` both
  behave correctly in either order. There is, however, a detectable consequence: over a run
  with no concurrent publication, `arrivals_counted == admitted + dropped`. If the tap sat
  after the gate, `arrivals_counted == admitted` and the dropped requests would be missing
  from the count. The harness can assert that identity; a unit test cannot, because it has
  no ingress site to instrument.
- **Actual shed proportions over the real RNG.** Statistical rather than exact, and blocked
  from unit testing by a hard fact rather than a preference: the enforcement TU has its own
  `adce_rng_tls`, which the test TU cannot reach or seed. This is exactly why the pure
  decision function must take the draw as a parameter — it is the only way any of the
  above is testable at all.
- **Per-arrival latency — measured, in `test/t_adce_latency.c`.** Reported per outcome,
  never asserted: a latency threshold on a shared CI runner is a flake generator, and this
  case runs in the per-edit gate. The only assertions are structural — that each fixture
  drove the outcome it claims — so the case cannot fail on a slow or loaded host.

  *Separating the gate from the clock read* needs no subtraction and no estimate. The
  separation is already in the API: `adce_enf_admit(ctx, now_ns)` takes the time as a
  parameter and never calls `adce_now_ns`, which belongs to the ingress site at step 1 of
  §3. Timing the gate times the gate. `adce_now_ns` is then measured separately with the
  same instrument, so both halves of per-arrival cost are measured numbers rather than one
  measured and one assumed at the 20–25 ns in §2.

  *Two instruments,* because the gate is smaller than the clock that must measure it —
  `CLOCK_MONOTONIC_RAW` quantises to ~41.7 ns on Apple Silicon. A **batch** pair around
  1024 calls amortises the clock away and resolves the central cost to picoseconds but
  cannot see a tail; a **paired** pair around one call has a quantised floor and a
  meaningless median but catches a microsecond outlier whole. The paired numbers are
  printed next to an empty-pair baseline — two clock reads with nothing between them — so
  the instrument's own distribution is visible beside the subject's.

  *Result*, Apple M3, arm64 (`ADCE_CACHELINE == 128`, `LDAR` + `DMB ISHLD`), strict `-O2`,
  280,576 calls per outcome:

  | | batch p50 | batch p99 | batch max | paired max |
  |---|---|---|---|---|
  | `admit` (longest: read, clamp, shed test, refill, take) | ~4.0 ns | ~8.1 ns | ~9.1 ns | 333 ns |
  | `shed` (shortest: read, clamp, shed test, return) | ~2.9 ns | ~3.0 ns | ~3.1 ns | 125 ns |
  | `limit` (read, clamp, shed test, refill, failed take) | ~3.8 ns | ~4.2 ns | ~4.2 ns | 125 ns |
  | `adce_now_ns`, for comparison — *not* part of the gate | ~13.0 ns | | | |
  | empty clock pair — the instrument's own floor | | | | 84 ns |

  Three things the table says that a mean would have hidden. The outcomes differ by ~40%
  and rank exactly as their code paths predict, so averaging them would have buried the
  one that does the most work. At p99 the gate is *below the clock's resolution* — the
  paired p99 is 42 ns for the gate and 42 ns for an empty pair alike — so there is no
  measurable p99 tail to report, which is a finding rather than a gap. And the paired
  maxima do sit above the 84 ns floor, so a real tail exists; at this sample count it is
  not separable from scheduler noise, and nothing here attributes it to a seqlock retry.

  The batch figure is a **lower bound**: a tight loop keeps `ctx` and the epoch line in L1
  with the branch predictors trained. A real ingress site interleaves request work and
  will sometimes take a cold line.

  Still open on this bullet: the x86_64 comparison. This is the arm64 number, which §2
  called the expensive case; whether TSO's plain `MOV` and no-op fence measurably beat
  `LDAR` + `DMB ISHLD` is answered by the same case running on the x86_64 CI runner, and
  is not yet recorded here.

  *A bound worth asserting, proposed and deliberately not folded in:* zero allocations and
  zero syscalls per arrival. That is structural rather than temporal, so it holds
  identically on a loaded runner and a quiet laptop and cannot flake — and it is what
  actually protects the numbers above, since 4 ns drifting to 6 ns is noise whereas a
  `malloc` or a futex appearing on the arrival path is a design regression that a timing
  threshold would hide inside the variance. It needs an allocator interposer and a syscall
  counter, neither of which exists in this repository yet.
- Closed-loop behaviour: whether pressure and admitted rate settle or oscillate.
- Observer death mid-flight: the watchdog trips, the fallback engages, and the system does
  not fail open.
- Multi-thread bucket behaviour and the `threads × rate` aggregate ceiling.

## 6. File layout

| Path | Contents | Why there |
| --- | --- | --- |
| `include/adce_enforce.h` | outcome enum, context type, constants, `adce_enf_should_shed()`, `adce_enf_decide()`, `adce_enf_admit()`, `adce_enf_thread_init()` — all inline | runs per arrival; the whole gate is hot path and must inline, like the tap |
| `test/t_adce_enforce.c` | the assertions in section 5 | first test file for this plane |
| `docs/enforcement-plane.md` | this document | |

**Resolved during implementation: `src/adce_enforce.c` does not exist.** This document
originally placed `adce_enf_thread_init` there, on the reasoning that a once-per-thread
entropy draw does not belong in a header. That is wrong, and the reason is the per-TU RNG
noted at the top of this document. `adce_enf_admit` is inline, so it draws from the
*calling* translation unit's `adce_rng_tls`. An out-of-line warmer in `src/adce_enforce.c`
would seed the *enforcement* TU's stream — a different object no ingress path ever touches
— leaving the lazy seed, and the `abort()` §1.4 exists to relocate, exactly where they
were. The function would advertise a guarantee it silently fails to provide, which is worse
than not having it. `adce_enf_thread_init` is therefore inline in the header alongside the
gate that shares its stream, and this plane has no out-of-line surface at all.

Test cases go in `test/t_adce_enforce.c` as `static int test_<name>(void)` with one external
forwarder each, registered in the runner table in `t_adce_platform.c` — the convention
established by `test/t_adce_observe.c`, required by the ran-tests guard in
`scripts/verify.sh`, and unchanged here.

## 7. What the first tests assert

In dependency order, each one failing loudly if the property above it is wrong:

1. `enf_shed_mapping` — exhaustive over the 65536 buckets at `p ∈ {0, 1, 16384, 32768,
   65535, 65536}`: the shed count equals `p` exactly. This is the assertion that catches a
   wrong shift, an off-by-one at either endpoint, or modulo bias.
2. `enf_shed_monotone` — the shedding set grows with `p` and never shrinks; and `p=0`
   admits every draw while `p=ADCE_PRESSURE_MAX` admits none.
3. `enf_read_clamp` — negative published pressure reads as maximal, not zero; out-of-range
   positive reads as maximal; in-range passes through.
4. `enf_stale_fallback` — an epoch at the timeout boundary is not stale, one past it is,
   and a stale read substitutes exactly `ADCE_ENF_STALE_PRESSURE` rather than 0 or maximal.
5. `enf_cold_start` — a zero-initialised epoch state takes the stale path on the first
   call, with no separate cold-start branch anywhere in the code.
6. `enf_bucket_ceiling` — over an injected `now_ns` sequence at `pressure == 0`, the
   admitted count is bounded by rate × elapsed + capacity. This is the assertion that the
   ceiling holds when the detector says everything is fine, which is the slow-ramp failure
   mode of §1.2.
7. `enf_determinism` — a recorded `(pressure, draw, now_ns)` sequence run twice yields a
   bit-identical outcome sequence.

## Open decisions, deliberately not defaulted

1. **The value of `ADCE_ENF_STALE_PRESSURE`.** Proposed `ADCE_Q16_ONE / 2`. This is an
   availability decision, not a technical one: it sets how hard a healthy system sheds
   during the one-second startup window and during any observer stall.
2. **Whether startup and observer-failure should share that constant.** §4 uses one value
   for both. A cold start is a known, bounded, expected condition; an observer that died
   under load is not. They may deserve different postures.
3. **`ADCE_ENF_RATE` and `ADCE_ENF_CAPACITY`.** Deployment-specific, and per-thread rather
   than global. Wrong defaults here are worse than no defaults, for the same reason T and N
   were left as tuning in the observation design.
4. **Whether xorshift128+ is adequate against an adversary who can observe outcomes.**
   §1.3 states the exact condition under which it is not. Replacing it is a real cost on
   the hot path and should not be paid on speculation.
5. **Whether the epoch read may be cached per epoch.** §2 argues it is safe only if the
   staleness verdict is recomputed per arrival, and that the saving is the cheaper half of
   the cost. Measure before deciding.
