# ADCE — Anomaly Detection & Stochastic Containment Engine

A two-stage admission control library in C11. An **Observation Plane** watches the arrival
rate and publishes a normalized containment score; an **Enforcement Plane** reads that score
at every arrival and decides admit, shed, or drop. Lock-free, allocation-free, no external
dependencies, one named compiler extension (`__int128`).

Shipping target is Linux x86_64. The gate runs natively on the development host, and arm64
is the stricter test bed for the seqlock because x86_64's TSO hides missing barriers that a
weakly-ordered target exposes.

## How to read this page

Every load-bearing claim below either **names the assertion that enforces it** or is
**explicitly marked unenforced**. You cannot run the suite before deciding whether to
believe the page, so a sentence with no assertion behind it is a claim you have no way to
check. Where a claim traces to a test, the test is named. Where it does not, that is said
outright.

Figures carry the number of observations behind them. Nothing here states a universal about
a quantity the suite only prints.

---

## What this does NOT protect against

These are properties of the design, not defects, and they are not tunable away. They are
first on the page because a reader who takes only the positives away has misunderstood what
the library is.

### The z-score detector is a fast-transient detector only

`pressure` is a z-score: a *relative* measure of deviation from an exponentially weighted
moving average of the recent past, with `N = 100` at `T = 10 ms`, so roughly a one-second
window. Two consequences follow from that and from nothing else.

**A sustained step becomes invisible.** After a step change the EWMA re-baselines onto the
new rate within roughly `N` epochs, `d` returns to zero, and pressure returns to
`ADCE_PRESSURE_MIN`. A permanent overload alarms briefly and is then, correctly by the
statistic's own definition, no longer an anomaly. The rig steps the rate by 8× at epoch 200
of 400 and the transient ends well before the run does.

*Partially enforced.* `loop_step_response_report` asserts the transient is bracketed
(`settled_at > LOOP_STEP_EPOCH`, `settled_at < LOOP_EPOCHS`) and that a window opened after
the measured end of the transient is exactly clear of it (`clear.pp == 0`, `clear.r_q16 == 0`,
`clear.dc == 0`) on a real step through the real `adce_obs_epoch_close`. The number of epochs
the transient lasts is **measured and never asserted**, deliberately: a threshold on it would
be a band with no derivation.

**A geometric ramp need not alarm at all.** On a ramp the EWMA reaches a steady solution in
which `z` is constant and *independent of the absolute rate*, so the detector is
scale-invariant while volume climbs without bound. Solving that fixed point at `N = 100` puts
the alarm threshold at `g* ≈ 8.98%` growth per epoch. **Anything doubling more slowly than
every 8.06 epochs — 80.6 ms — passes the detector entirely, at any amplitude, forever.**

*Enforced at the ends, not at the value.* `loop_ramp_below_threshold` asserts that a ramp at
`g = 0.02` holds pressure at `ADCE_PRESSURE_MIN` across a derived window **while offered
volume grows at least 100×** — the growth half is asserted separately, because a stopped rig
also holds pressure at the floor. `loop_ramp_above_threshold` asserts the mirror at
`g = 0.20`. `loop_ramp_teeth` feeds each predicate the data that must break it, including a
flat load, which passes the floor predicate and is rejected only by the growth half.

The value `g* = 8.98%` itself is **not asserted anywhere**. It is derived in
`docs/closed-loop-harness.md` §2B and confronted with the code in
`loop_ramp_fixed_point_report`, which asserts nothing but the return code.

**What follows for a deployment.** Outside a window of roughly `N` epochs after a fast change,
`ADCE_ENF_RATE_Q16_PER_NS` and `ADCE_ENF_CAPACITY_Q16` are the only thing standing between
the system and unbounded volume. The token bucket is the sole defence against sustained or
slowly-growing load. This is why the bucket's rate must never become a function of pressure:
at pressure zero the bucket would open fully and the slow ramp would pass unimpeded.

### The settle band is blocked, not open

There is no criterion in this repository for deciding that a pressure trajectory has settled,
and the reason is that the two candidate metrics cannot supply one.

`DC` counts sign changes and `R` is `TV/PP`, a ratio. **Both are amplitude-blind by
construction**, so neither can be given a deadband from its own definition.

*Asserted, in `loop_settle_metrics_teeth`.* A full-scale square wave and a 1-LSB dither
produce identical `DC` and identical `R`, and the case asserts exactly that:
`dither.dc == sq.dc`, `dither.r_q16 == sq.r_q16`, and `sq.pp == dither.pp * ADCE_PRESSURE_MAX`
— the two are separated by a factor of 65536 in `PP` and by nothing else.

`R` is separately compromised. When a window mixes a one-time transient with a persistent
cycle, `PP` is set by the transient while `TV` is shared, so `R` deflates. The case asserts
that at a cycle amplitude of 87 of 65536 the full window reports `R < 1.5` while the trailing
window still reports the true `R = 199` — a factor over 100, asserted as
`tail.r_q16 > 100 * r_hidden_full`. No threshold that still admits a settling trajectory can
separate `R < 1.5` from the monotone decay's exact `1.0`, which the same case asserts as
`dec.r_q16 == ADCE_Q16_ONE`.

The only quantity that separates a full-scale limit cycle from quantization dither is `PP`,
and a threshold on `PP` **is** the settle band `B`. So the deadband question reduces to `B`
rather than being a second open number, and `B` has no derivation. **No threshold was
invented.** `loop_settle_metrics_teeth` asserts metric *values* on fabricated sequences and
never a verdict.

What *is* settled is narrower and is fully asserted: the internal loop is open over time.
`loop_draw_invariance` asserts bit-identical pressure trajectories across two different draw
streams over 400 epochs, with the verdict checksums asserted to *differ* so the result cannot
be vacuous. `loop_inverted_draw_dependence` asserts that the inverted ordering diverges. The
dynamics question — whether the loop settles or limit-cycles — has **no executable evidence
in either direction**, and the rig cannot supply it: it produces no limit cycle to measure,
which is why the teeth have to fabricate one.

### The contended cost of the arrival tap is zero-evidence

`adce_obs_tap` is a relaxed `fetch_add` on a single cache line shared by every ingress
thread. Each increment needs exclusive ownership of that line and pays a cross-core transfer
under contention, so it is structurally the one per-arrival term that does not scale.

**It has never been measured under contention.** This was verified rather than assumed:
`adce_obs_tap` appears **nowhere** in `test/t_adce_latency.c`, and that file creates **no
threads at all**. The evidence is zero, not weak.

That matters because `docs/closed-loop-harness.md` §4 publishes a per-thread bound of
**27–59 M arrivals/s** derived from the gate plus the clock read. **That figure is an upper
bound the tap may not permit**, and the document says so. Running the contended measurement
can invalidate it by an order of magnitude.

---

## The one misreading to avoid: `threads × rate` is not a global ceiling

Each ingress thread carries its **own private, non-atomic bucket**. The aggregate admitted
ceiling is therefore `threads * rate`, and the deployment tuning block in
`include/adce_enforce.h` names the constants for the per-thread quantity precisely so this is
not discovered late.

The stronger version, which is what actually bites: the aggregate conservation identity is

```
sum(K*A_i) = sum(C_i) + R*sum(span_i) - R*sum(d0_i) - sum(tau_i)
```

and this **collapses to a single global span only if every thread's span is identical**. Four
threads starting and stopping at different real times do not satisfy that.

*Asserted, in `test_harness_concurrent`.* The per-thread form is asserted for every thread via
`harness_bucket_identity`, and the aggregate is asserted as
`agg_supply == agg_spend` with the capacity term summed as `threads * C` and the span summed
per thread. The comparison against a collapsed global span is **measured, not asserted**: on
one run it was off by 52 admissions. One observation, stated as one.

A deployment that sizes on a global ceiling sizes wrong.

---

## Using it

Three steps at an ingress site, in this order. The order is the project's core principle.

```c
#include "adce_observe.h"
#include "adce_enforce.h"

static adce_obs_counter_t g_arrivals;    /* one counter, every ingress thread taps it */
static adce_epoch_state_t g_epoch_state; /* written by the Observation Plane, read here */
static adce_enf_ctx_t     g_enforce;     /* per-thread, never shared */

/* Once per ingress thread, off the request path. */
static void ingress_thread_start(void) {
    adce_enf_thread_init(&g_enforce, &g_epoch_state, adce_now_ns());
}

static int ingress_handle(struct conn *c) {
    uint64_t now_ns;
    adce_enf_outcome_t outcome;

    /* 1. TAP -- unconditional, and FIRST. Every arrival is counted, including
     *    the ones the gate is about to drop. Nothing may return before this
     *    line: not a parse failure, not an early exit for malformed input. */
    adce_obs_tap(&g_arrivals);

    /* 2. GATE -- one clock read serves the staleness check and the refill. */
    now_ns = adce_now_ns();
    outcome = adce_enf_admit(&g_enforce, now_ns);
    if (outcome != ADCE_ENF_ADMIT) {
        return conn_reject(c, outcome);
    }

    /* 3. WORK -- reached only by admitted arrivals. */
    return conn_serve(c);
}
```

**Why the tap must precede the gate.** If the gate runs first, the detector counts only
*admitted* arrivals, so containment suppresses its own input signal and the loop closes on
itself.

*Asserted in both directions.* `test_harness_tap_before_gate` asserts
`site.tapped == HARNESS_OFFERED` and that the real Observation Plane counter agrees with the
audit counter exactly. `test_harness_tap_after_gate` is the counter-proof: it **passes by
asserting the identity fails**, and pins the failure's exact shape — `tapped == admitted`,
and the real counter reporting that 4096 dropped arrivals never happened. If the inverted arm
ever stops failing, the first case has lost its teeth.

The observer half is optional by construction. A consumer owning its own cadence calls
`adce_obs_epoch_close` directly and never links `src/adce_obs_thread.c`.

## Build

No build system. Two gate scripts compile everything directly.

```
./scripts/verify.sh          # strict -O2, ASan+UBSan, TSan -- all three must pass
./scripts/verify-linux-gcc.sh # GCC 14 on linux/amd64 + linux/arm64, plus a pinned profile
```

Quick syntax check of one translation unit, without linking — the runner table forwards to
cases defined in the other test files, so a link of this file alone will not resolve:

```
cc -std=c11 -O2 -Wall -Wextra -Werror -Iinclude -fsyntax-only test/t_adce_platform.c
```

`ADCE_REPEAT=n` re-runs each profile's binary `n` times without rebuilding.

## Tuning constants

| constant | value | meaning |
|---|---|---|
| `ADCE_OBS_EPOCH_NS` | 10 ms | epoch length `T` |
| `ADCE_OBS_WINDOW_N` | 100 | EWMA window `N` |
| `ADCE_OBS_Z_LO` / `Z_HI` | 3 / 8 | squash knees |
| `ADCE_OBS_WARMUP_EPOCHS` | 100 | 1 s during which nothing publishes |
| `ADCE_ADVICE_TIMEOUT_NS` | 50 ms | staleness watchdog |
| `ADCE_ENF_RATE_Q16_PER_NS` | 7 | **per thread**, ≈106,800 admissions/s |
| `ADCE_ENF_CAPACITY_Q16` | 4096 × `ONE` | **per thread**, burst allowance |
| `ADCE_ENF_STALE_PRESSURE` | 0.5 | fallback when the epoch reads stale |

**`ADCE_OBS_WINDOW_N` must stay below 125**, and this one is enforced at compile time. Above
that a steep enough geometric ramp can saturate the squash, which changes a qualitative
property of the detector rather than how much it smooths. The `_Static_assert` in
`include/adce_observe.h` encodes `2*z_hi^2*(N-1) > (N+1)^2` in pure integer arithmetic, and it
is written against `ADCE_OBS_Z_HI_INT` rather than a literal, so retuning `z_hi` moves the
bound with it. The bound depends on which variance recurrence `src/adce_observe.c` uses; the
other common form gives 127, and using it here would be wrong by two in the permissive
direction.

**No test asserts the values of the enforcement constants.** The tests assert that the ceiling
*holds*, so retuning cannot make a test lie about the property being protected.

---

## What is asserted

48 cases, run under three profiles. `scripts/verify.sh` reads the expected set out of the
**source** — every `static int test_<name>(void)` definition — not out of the binary, so a
case that compiles but is never wired into the runner table turns the gate red.

| claim | assertion |
|---|---|
| Every arrival reaches the detector, drops included | `harness_tap_before_gate`: `tapped == HARNESS_OFFERED` |
| The inverted ordering breaks that, in a specific shape | `harness_tap_after_gate`: `!site_identity_holds`, `tapped == admitted` |
| The internal loop is open over time | `loop_draw_invariance`: bit-identical trajectories, 400 epochs, two draw streams; verdicts asserted to differ |
| Inverting the ordering closes it | `loop_inverted_draw_dependence`: trajectories differ; `tapped` strictly below offered |
| Same seed, same everything | `loop_synthetic_determinism`: trajectory, totals, and the per-arrival verdict checksum |
| A slow ramp does not alarm while volume grows | `loop_ramp_below_threshold`: pressure pinned at MIN, growth ≥ 100× asserted separately |
| A fast ramp does alarm, and cannot saturate | `loop_ramp_above_threshold`: pressure above MIN and strictly below `ADCE_PRESSURE_MAX` |
| Both ramp predicates have teeth | `loop_ramp_teeth`: three mutations, including a flat load |
| Bucket conservation, single-threaded, both clocks | `loop_bucket_conservation`: `K*A == C + R*span - R*d0 - tau_final` |
| The identity predicate has teeth | `loop_bucket_identity_teeth`: one over, one under, over-admission shape, and a stall |
| The clamp regime, constructed rather than waited for | `loop_bucket_clamp_regime`: `clamp_hits == 200` exactly, derived from two `_Static_assert`s |
| Bucket conservation under four live threads | `harness_concurrent`: per-thread and aggregate, real clocks |
| The per-thread ceiling holds under concurrency | `harness_concurrent`: `admitted <= ceiling` per thread |
| Stale reads classify into exactly three routes | `enf_stale_route_identity` through the shipped gate; `harness_concurrent` asserts `torn + future + aged == stale` under live concurrency |
| The classifier decides the same verdict as the predicate it replaced | `enf_stale_route_equivalence`, over a grid straddling the timeout boundary and the unsigned wrap |
| Post-warmup, no aged reads | `harness_concurrent` and `harness_stale_posture`: `aged == 0` over a window opened at the confirmed first publication |
| Blind phases drive the aged counter fully | `harness_check_blind_phase`: `aged == stale`, `torn == future == 0` |
| The stale posture sheds near half | `harness_check_blind_phase`: 420–580 permille band, with separation ≥ 150 from the live band asserted |
| The split assertion has teeth | `harness_stale_split_teeth`: `aged=5` against a bound of 31 passes the old predicate and fails the new one |
| `DC` and `R` cannot separate a limit cycle from dither | `loop_settle_metrics_teeth`: `dither.dc == sq.dc`, `dither.r_q16 == sq.r_q16` |
| `N < 125` | `_Static_assert` in `include/adce_observe.h`, compile time |

Two fail-closed contracts, both asserted in `enf_stale_route_classify`, and both carried by
`adce_enf_classify_stale` — which is the only staleness code the gate runs.

A torn `adce_epoch_read` counts as stale, because no snapshot means no advice, and torn
dominates every timestamp including ones that would otherwise read fresh. A *future*
`observed_at_ns` is stale too: a reader that cannot order the publication it read against the
clock it read has no coherent view of time. `adce_enf_classify_stale` decides that with an
explicit `observed_at_ns > now_ns` branch, taken **before** the subtraction, which is why the
case asserts that `UINT64_MAX` classifies as future rather than aged. **Delete that branch and
the case fails**, which is how this sentence is known to be a claim rather than a description.

An earlier version of this paragraph credited the unsigned wrap in `adce_epoch_is_stale`
instead, and warned that replacing the wrap would turn the case fail-open. That was false. The
gate does not call `adce_epoch_is_stale` — it has zero call sites outside the test suite — so
replacing its wrap changes nothing the gate does, and `enf_stale_route_classify` stays green
when you do. The sentence named a real function that behaves exactly as described and was
still wrong about the system. See the entry on that in `CLAUDE.md`.

## What is measured and deliberately not asserted

A threshold on any of these would be a band with no derivation, which is the failure mode the
project exists to avoid. Reporting a measurement and claiming a law about it are different
acts.

- **Per-arrival gate cost.** `t_adce_latency.c` asserts only that each fixture drove the
  outcome it claims. Single-run figures from CI run 33772329101, both legs native, clang
  18.1.3, strict `-O2`: gate 3.86–4.89 ns on x86_64 and 5.23–6.09 ns on arm64 depending on
  outcome; `adce_now_ns` 16.9 ns on x86_64 and 30.6 ns on arm64. The batch figure is a lower
  bound — a tight loop keeps the context in L1.
- **The §2B fixed point.** `loop_ramp_fixed_point_report` asserts nothing but the return code.
  Measured relative error against the code's own recurrence was 1.6e-12 at `g = 0.02` and
  5.6e-14 at `g = 0.20`, against 1.005e-2 for the other candidate recurrence — which is what
  makes the `N < 125` bound pinned to the right number rather than to 127.
- **Step response.** `loop_step_response_report` reports every step-response number.
- **The bucket floor form** `A == floor((C + R*span)/K)`. Reported beside the asserted
  conservation form and never asserted: it needs `L + tau_final < K`, and `tau_final` is
  effectively uniform over `[0,K)`. It mismatched **5 of 400** real-clock runs where the
  conservation form mismatched zero.
- **Clamp counts in the real-clock arm.** 24 of 25 executions showed four clamps, one showed
  five. The identity never depended on the count. The *synthetic* clamp regime is a different
  matter: `loop_bucket_clamp_regime` constructs the regime so the count is derived and
  asserted as an equality on every run on every host.

## What is not verified

In descending order of how much running it would change a decision.

1. **The contended cost of `adce_obs_tap`** — zero evidence, as above. Can invalidate a
   published bound.
2. **The Darwin half of `adce_platform_get_entropy`.** The `getentropy` chunking loop has no
   automated coverage at all. CI runs `ubuntu-24.04` and `ubuntu-24.04-arm` only, so every CI
   job takes the `getrandom` branch, and macOS — the platform the per-edit gate runs on — is
   in no job. This is the only whole code path unexecuted by any automated gate.
3. **The seqlock retry path has never been timed.** Every latency figure comes from a fixture
   with no concurrent publication, so `adce_epoch_read` never retried. The path *is* executed
   under `harness_concurrent`, which records nonzero torn reads; it is only its cost that is
   unmeasured.
4. **GCC's ThreadSanitizer runs nowhere.** The GCC profile is ASan+UBSan only, so every race
   result in this project is Clang's. This ranks low on purpose: GCC vendors LLVM's TSan
   rather than implementing its own, so a second front end over near-identical detection is
   worth having and worth little.
5. **The settle band.** Blocked rather than unattempted, for the reason given above. There is
   no experiment to run.

The `abort()` in `adce_rng_seed` has never executed and cannot without fault injection. That
is by design: a PRNG seeded from a failed or partial entropy draw is predictable, and every
downstream containment decision would inherit that, so there is deliberately no degraded
seeding path.

**One entry in the project's own unverified list is stale in the under-claiming direction.**
It records the aggregate ceiling under real concurrency as "runnable, untested, and the next
task". That landed: `harness_concurrent` asserts `harness_bucket_identity` per thread and the
two-sided aggregate identity. Read the tests, not the list.

---

## What the verification apparatus rules out, and what it does not

It is a gate, not a virtue. What follows is what it catches and what it demonstrably misses.

**Three profiles, all required.** Strict `-O2`, ASan+UBSan, TSan. The strict profile is not
redundant with the sanitized ones, and this was learned the hard way: the only timing bug this
project has ever found was a **logical** phase-accounting race, which no ThreadSanitizer of
any vendor can see. It surfaced under strict `-O2` because ASan and TSan dilate execution
enough to mask that class. It survived four green runs first.

**A green local gate is weaker evidence than it looks for any timing-dependent derivation.**
In PR #16 the conservation identity carried a precondition inherited from a single-threaded
rig. `verify.sh` was green on it across nine executions under `ADCE_REPEAT=3`, all three
profiles, and `verify-linux-gcc.sh` was green locally too. **`shipping-target` on CI failed
it**, off by 401,047 Q16 — about six admissions. The derivation was simply wrong, and wrong in
the permissive direction. It was not a portability defect: same source, same compiler family,
same architecture. It was reachable only on a host whose thread scheduling produced a large
enough early gap. Any assertion whose correctness depends on a timing regime is only as sound
as the widest set of schedules it has been evaluated under.

**The gate cannot check its own repeat count.** `ADCE_REPEAT=10` on the nightly is a round
number. Sizing a repeat count to a detection probability needs a point estimate of the
per-execution failure rate, a point estimate needs observed failures, and a green suite yields
only an upper bound. The re-derivation was attempted and failed. A fault-injection campaign
reinstating the original race produced **0 failures in 100 executions** under each of three
configurations — 300 executions of a deliberately racy build, zero reproductions. This project
can no longer reproduce the only timing bug it has ever found.

**`gh pr checks` shows four green checks and only three are required.** Verified against the
ruleset rather than the PR page: the required contexts are exactly `sanitizers (ubuntu-24.04)`,
`sanitizers (ubuntu-24.04-arm)` and `shipping-target`. The fourth is CodeRabbit, and on every
pull request in this repository it reports `Review skipped: manual review required for this
OSS repository` — **it passes green having reviewed nothing.** `required_approving_review_count`
is **0**, so what the ruleset enforces is the pull-request path and the checks, not human
review. A reader counting green ticks would conclude the bar is four checks including a
review; the actual bar is three checks and no review.

**`strict_required_status_checks_policy` is false**, so a pull request may merge on checks
that ran against a branch behind `main`.

---

## Layout

| path | contents |
|---|---|
| `include/adce_platform.h` | header-only platform layer: Q16.16 lane, RNG, seqlock, time |
| `include/adce_observe.h` | Observation Plane types, the single tuning block, the inline tap |
| `src/adce_observe.c` | the producer, off the arrival path |
| `include/adce_obs_thread.h`, `src/adce_obs_thread.c` | epoch cadence and writer ownership; optional |
| `include/adce_enforce.h` | the **entire** Enforcement Plane, inline, with no `.c` file |
| `test/t_adce_platform.c` | `main()` and the single runner table |
| `test/t_adce_loop.c` | the closed-loop rig: 13 cases |
| `test/t_adce_harness.c` | the integration harness; the only file testing a call *order* |
| `test/t_adce_latency.c` | per-arrival cost, measured and never asserted |
| `docs/` | three design documents, each written before the code it describes |

The Enforcement Plane's absence from `src/` is a resolved decision rather than a gap.
`adce_enf_admit` is inline, so it draws from the **calling** translation unit's thread-local
RNG; an out-of-line `adce_enf_thread_init` would warm a different stream than the one the gate
draws from, silently. Every enforcement function reachable from a test takes its
nondeterminism as a parameter — `now_ns` and the RNG `draw` — for the same reason: a test
translation unit cannot observe or seed the stream an enforcement translation unit uses. That
is structural, not stylistic.
