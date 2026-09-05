# Closed-loop harness — design

Status: design only. No implementation exists. This document exists because
`docs/enforcement-plane.md` §5 and `docs/observation-plane.md` §5 have both listed
closed-loop behaviour as needing a harness since the planes were written, and it is the
one item on either list that is a claim about the DESIGN rather than about a function.

Its own file rather than a section in either plane's document, because the claim is the
composition of the two: the Observation Plane measures, the Enforcement Plane actuates, and
whether the pair settles or oscillates is a property of neither alone.

## 0. What is claimed, and what has actually been shown

`observation-plane.md` §2 makes the claim in two halves:

> Tapping after the gate closes an actuation-feedback loop [...] Two consequences:
> limit-cycle oscillation around the threshold rather than settling; and, worse, an
> attacker who pushes just past the threshold gets the system to self-soothe.

`test_harness_tap_before_gate` and `test_harness_tap_after_gate` prove the ORDERING — that
the tap statement executes before the gate statement at a site that has both, and that the
identity `tapped == admitted + dropped` distinguishes the two. That is a static property of
a call site, established over a frozen `now_ns` with no observer running and no epoch ever
closing. **Nothing in this repository has ever run the detector and the actuator against
each other over time.** The dynamics half of §2 — "limit-cycle oscillation rather than
settling" — has no executable evidence in either direction.

This document says what evidence would look like, and is deliberate about the part that
cannot be evidence: this project does not gate on timing, and §5 below says plainly which
of the properties here are assertions and which are measurements.

## 1. Where the loop is open, and where it is not

    offered ──▶ [TAP] ──▶ [GATE] ──▶ admitted ──▶ downstream ──▶ client
                  │          ▲                                     │
                  ▼          │                                     ▼
             counter ──▶ EWMA ──▶ squash ──▶ pressure         (retries? backoff?
                     (once per T)                              fixed in-flight?)
                                                                    │
                                                                    ▼
                                                            back to `offered`

There are two candidate loops and they are not the same loop.

**The internal loop** runs counter → EWMA → pressure → gate → counter. Tap placement is
what cuts it: with the tap first, the counter records OFFERED arrivals, so `n_k` is
independent of every drop decision the gate makes, and the detector's input does not depend
on the detector's output. With the tap after the gate, `n_k` is the admitted count and the
loop is closed inside the process. This loop is entirely under this repository's control.

**The external loop** runs through the client: a dropped request may be retried, may cause
backoff, or may free a slot in a fixed-concurrency client that immediately issues another.
Tap placement does nothing to this loop. A client model is required to close it, and this
repository contains no client model and no basis for choosing one.

Conflating the two is the failure this section exists to prevent, and it decides everything
below: **the internal loop yields assertions and the external loop yields measurements.**
"The design settles" is a statement about the external loop, so §5 says what it would take
to earn it and does not pretend the internal-loop evidence is that.

## 2. Question 1 — what load pattern actually tests the claim

Six patterns. Each entry says what it can show, what it cannot, and the failure mode
choosing it prevents.

### A. Step — below threshold to well above it

Offered rate jumps from `r` to `m·r` at a known epoch and holds.

*Shows:* the transient. How far pressure rises on the step, whether it overshoots, and how
many epochs it takes to come back down.

*Cannot show:* settling at a nonzero operating point, because there is none to settle at.
The detector is a CHANGE detector: after the step the EWMA re-baselines onto `m·r` within
roughly `N` epochs, `d` returns to zero, and pressure returns to `ADCE_PRESSURE_MIN`. A
sustained overload becomes invisible. That is the §1.2 slow-ramp argument in step form, and
it is a finding to print rather than a defect.

*Failure mode prevented:* a rig that never leaves the linear region of the squash and so
never exercises saturation, the clamp, or the recovery path.

### B. Slow geometric ramp — the case §1.2 says the bucket exists for

Offered rate `r_k = r_0 (1+g)^k` with `g` small.

This one has a derivation rather than a guess, and it is stronger than §1.2 states.

Let `a = ADCE_OBS_ALPHA = 2/(N+1)`. On a geometric ramp the EWMA reaches a steady solution
`mu_k = c·r_k` with `c = a(1+g)/(g+a)`, so the deviation `d_k = r_k·g/(g+a)` is itself
geometric with the same ratio, and the variance recurrence has the matching solution
`var_k = v·d_k²` with

    v = (1-a)a / (1 - (1-a)/(1+g)^2)      and      z = d_k / sqrt(var_k) = 1/sqrt(v)

`z` on a geometric ramp is therefore **constant, and independent of the absolute rate** —
the detector is scale-invariant, which is exactly why volume can climb without bound while
`z` sits still. At N = 100:

| g (per epoch) | doubling time | z | pressure |
|---|---|---|---|
| 0.001 | 693.5 epochs (6.9 s) | 1.06 | 0 |
| 0.01 | 69.7 epochs (0.70 s) | 1.42 | 0 |
| 0.02 | 35.0 epochs (0.35 s) | 1.73 | 0 |
| 0.05 | 14.2 epochs (0.14 s) | 2.39 | 0 |
| **0.0898** | **8.06 epochs (81 ms)** | **3.00** | **0 — at the threshold** |
| 0.2 | 3.8 epochs | 4.06 | 0.21 |
| g → ∞ | — | **7.178** | **0.836** |

Doubling times are `ln2 / ln(1+g)`, not `ln2 / g`. The small-`g` approximation is what the
first version of this table used and it is 4.4% low at `g*` — 7.7 epochs against the true
8.06 — which is the wrong direction, since it overstates how fast a ramp must climb before
it is safe. The error is negligible in the first two rows and is not in the row that
matters.

Two derived numbers fall out, neither of which is written down anywhere in this repository:

- **`g* ≈ 8.98% per epoch` is the alarm threshold for exponential growth.** Any ramp
  doubling more slowly than every 8.06 epochs — 81 ms — passes the detector entirely, at any
  amplitude, forever. That is §1.2's claim as a number.
- **`sup z = 1/sqrt((1-a)a) = 7.178 < z_hi = 8`, so a geometric ramp can never saturate the
  squash at all.** The steepest conceivable exponential ramp caps pressure at 0.836 of
  maximum. And this is a property of the current tuning, not of the shape: `sup z` crosses
  `z_hi` at **N = 125**. A future retune of N past 125 silently changes whether a ramp can
  ever reach full containment. Recorded here because it is invisible at the call site and
  no test would catch it.

  The crossover depends on which variance recurrence is used, so it was read out of the
  code rather than assumed. `src/adce_observe.c` computes `var = (1-a)*(var + a*d*d)`, with
  the `(1-a)` outside the bracket, giving `sup z = 1/sqrt((1-a)a)` and 125. The other
  common form, `var = (1-a)*var + a*d*d`, gives `1/sqrt(a)` and exactly 127. 125 is this
  codebase's number; 127 would be off by two in the permissive direction.

*Shows:* pressure pinned at `ADCE_PRESSURE_MIN` while offered volume grows by two orders of
magnitude, and the token bucket holding the ceiling alone throughout.

*Cannot show:* any dynamics. The loop is never excited; nothing moves.

*Failure mode prevented:* believing the detector bounds absolute volume. It is the negative
control for the whole rig.

### C. Fast ramp — above `g*`

The same shape with `g` above 8.98%.

*Failure mode prevented:* pattern B being a vacuous negative control. A rig where pressure
is always zero proves nothing about a detector, exactly as a harness that only ever runs
the correct ingress ordering proves nothing about ordering. C is B's teeth: it shows the
threshold is real and two-sided, and it does so from the same derivation rather than from a
tuned observation.

### D. Periodic drive — square wave, period swept

Offered rate alternating between two levels with period `P` epochs, `P` swept across a
range spanning the EWMA time constant (`1/a ≈ 50` epochs) and the epoch period itself.

*Shows:* forced response, and resonance if the loop has any. If some `P` produces a
disproportionate pressure amplitude, that period is the loop's natural frequency and is
where a limit cycle would live.

*Cannot show:* the absence of a limit cycle at unswept periods. A sweep is a search, and a
negative result from a search bounds only what was searched.

*Failure mode prevented:* concluding "it does not oscillate" from a single load shape. This
is the same argument that produced the inverted ingress site and every teeth proof since.

### E. Responsive client — the only pattern that closes the external loop

Offered rate is a function of the gate's verdicts. Two models worth naming, both standard
and neither derivable from anything in this repository:

- *fixed in-flight:* the client holds `C` requests outstanding; a verdict — admit or drop —
  frees a slot immediately, so drops INCREASE the offered rate.
- *backoff:* a dropped request pauses that client for `B` epochs, so drops DECREASE the
  offered rate.

The two have opposite signs, which is the point: the external loop's stability depends on
the client, and this project gets to choose neither. Under a responsive client, the offered
sequence depends on the draws, so nothing here is bit-reproducible and nothing is
assertable — see §5.

*Failure mode prevented:* claiming closed-loop settling from an open-loop experiment.

### F. Draw-swap pairs — the experiment, not a pattern

Any of A–D, run twice against the same offered sequence with two different draw streams.
This is where the internal-loop assertions come from; §6 states them.

## 3. Question 2 — what "settles" means numerically

Pressure is a `adce_q16_t` in `[0, 65536]` that changes **only** at a publication, i.e.
once per `ADCE_OBS_EPOCH_NS`. The rig drives `adce_obs_epoch_close` itself, so it samples
the trajectory exactly once per publication. That is not a sampling-rate choice, it is the
signal's own update rate: there is nothing between two publications to alias, and the
fastest representable cycle is period 2 epochs (20 ms). **The "merely looks calm at one
sampling rate" objection does not apply to a rig that samples at the update rate**, and it
would apply to a real-time poller, which is the second reason §4 puts the assertions in the
synthetic rig.

Over a trailing window `W` of the published sequence `p_0 … p_{K-1}`:

    PP(W) = max(p) - min(p)                       peak-to-peak
    TV(W) = sum |p_{i+1} - p_i|                   total variation
    DC(W) = sign changes in the nonzero diffs     direction changes
    R(W)  = TV / PP        (R := 0 when PP == 0)  excursion ratio

**`R` is the observable that separates settling from a slow limit cycle, and variance is
not.** The argument is geometric rather than statistical:

- A monotone approach to a limit — settling after a step — traverses its range once.
  `TV = PP`, so `R = 1` exactly, and `DC = 0`.
- A cycle traverses its range twice per period. `R ≈ 2·(cycles in W)`, and `DC ≈ 2·(cycles
  in W)`. So `R/2` **counts the cycles** and `2W/R` **is the period in epochs** — the rig
  reports a number a reader can check against the load's own period.
- Variance conflates the two: a monotone drift and a cycle of the same amplitude have
  comparable variance, and a slow cycle observed over less than one period has small
  variance and small `PP` while being a cycle. `R` is dimensionless and scale-free, so it
  does not care about the amplitude, and `DC` does not care about the window length.

The residual limitation is honest and unavoidable: a cycle whose period exceeds `W` shows
`R ≈ 1` and is indistinguishable from a drift. The defence is not a cleverer statistic, it
is to state `W` and therefore state the slowest cycle the run can see. `W` should be at
least `4/a ≈ 200` epochs (2 s), four EWMA time constants, so any cycle driven by the
detector's own memory completes several times inside it.

"Settled over `W`" is then `PP(W) <= B` and `DC(W) <= D`. **`B` and `D` have no derivation,
and §5 is where that is dealt with rather than papered over.**

## 4. Question 3 — generating load deterministically enough to assert on

Yes, the ingress can be driven from a synthetic clock, and further than
`adce_obs_epoch_close` alone allows. Three parameters are already injectable, each for a
stated reason that this rig now collects:

| injection point | already a parameter because |
|---|---|
| `adce_obs_epoch_close(ctx, observed_at_ns)` | `observation-plane.md` §5: "`adce_now_ns()` is not injectable [...] or none of the above is deterministically testable" |
| `adce_enf_decide(ctx, now_ns, draw)` | `enforcement-plane.md` §5: the pure decision function must take the draw, since the enforcement TU's `adce_rng_tls` is unreachable from a test TU |
| the arrival sequence | the rig's own loop counter |

So the rig needs **no new API and no change to `src/` or `include/`**. It is a single
thread that, for each model epoch `k`: calls `ingress(site, now)` for `n_k` arrivals with
`now` advancing by a fixed synthetic step, then calls `adce_obs_epoch_close(ctx, now)`, then
records the published pressure. Model time is a `uint64_t` the rig owns. Nothing sleeps,
nothing races, and two runs of the same configuration produce bit-identical output.

**What that costs in fidelity**, stated as four separate losses rather than one hedge:

1. **No concurrency.** One thread, so no seqlock straddle: `torn` and `future` reads cannot
   occur and the stale routes are not exercised. Already covered by
   `test_harness_concurrent` and `test_harness_stale_posture`; this rig deliberately does
   not re-cover it.
2. **No scheduler jitter in the arrival rate.** `n_k` is exactly what the pattern says. The
   real system's `var` is dominated by jitter, so the synthetic rig's `sigma` is far smaller
   than production's and its `z` is correspondingly larger for the same load shape.
   **Absolute pressure values from this rig do not predict production.** What transfers is
   the SHAPE — settling versus cycling — and the derived thresholds of §2B, which are
   properties of the recurrence rather than of the noise.
3. **Epochs are exactly `T` apart.** No late epochs, no missed-epoch discard, no ageing.
   The watchdog never trips, which is correct here: `test_harness_stale_posture` owns that.
4. **The ingress site is not the shipped recipe.** `ingress_correct` calls
   `adce_enf_admit`, which draws from the calling TU's real stream; a rig that injects the
   draw must call `adce_enf_decide` instead. `adce_enf_admit` is a one-line wrapper over
   `adce_enf_decide(ctx, now, adce_rng_next())`, so the gate under test is identical and
   only the draw's provenance differs — but it is a deviation and the rig's site must be a
   separate function from `ingress_correct`/`ingress_inverted`, not a modification of them.

**The real-time arm keeps the fidelity the synthetic rig gives up**, and is measured rather
than asserted. Its budget is now a number rather than a guess: `enforcement-plane.md` §5
measures the gate at 3.9–6.1 ns per call across both architectures and `adce_now_ns` at
13.0 ns (M3), 16.9 ns (x86_64 CI) and 30.6 ns (arm64 CI) — the arm64 CI figure is the one
to size against, not the 12–17 ns range the other two hosts suggest. So a real-time ingress
arrival costs roughly 17–37 ns depending on host, bounding one thread at 27–59 M arrivals/s
before any request work. **One per-arrival cost in that sum has never been measured: the
tap.** `adce_obs_tap` is a relaxed `fetch_add` on a single cache line shared by every
ingress thread, and under four-thread contention it can plausibly dominate both the gate
and the clock. The real-time arm's offered rate must therefore be treated as an upper bound
that the tap may not permit, and measuring the contended tap is a prerequisite for claiming
otherwise.

The synthetic rig skips the clock read entirely, so it costs roughly the gate alone: ~5 ns
per arrival. A 400-epoch run at 200,000 arrivals per model epoch is 8·10^7 arrivals, about
0.4 s of wall time, and models 20 M arrivals/s — two orders of magnitude above the
per-thread bucket rate of ~107 k/s, so all three outcomes occur throughout. That is what
makes the sweep of §2D affordable in the per-edit gate.

## 5. Questions 4 and 5 — what a cycle looks like, and what may be asserted

### What a limit cycle looks like in the printed output

The rig prints, per configuration: a decimated pressure trajectory, and for the trailing
window `PP`, `TV`, `DC`, `R`, the implied period `2W/R`, and the admitted count per epoch
beside it. A limit cycle is unmistakable in four places at once:

- pressure alternating between `ADCE_PRESSURE_MAX` and `ADCE_PRESSURE_MIN` rather than
  resting between them — the threshold cliff §1.1 says proportional shedding removes;
- `R` well above 1, and `DC` growing linearly with the window;
- an implied period `2W/R` that is stable as `W` is lengthened (a cycle) rather than
  growing with it (a drift);
- the admitted-rate trace in antiphase with pressure — which is the self-soothing signature
  specifically, and is the one a bare pressure trace would not show.

### What would rule it out — and the reason a healthy-case-only rig proves nothing

Two counterfactuals, both required:

1. **The inverted arm.** Every configuration runs against both `ingress_correct` and
   `ingress_inverted`. If the inverted arm does not diverge from the correct one under a
   given pattern, that pattern does not excite the internal loop and its green result on
   the correct arm means nothing. This is the same instrument that made
   `test_harness_tap_after_gate` worth having.
2. **The metric's own teeth**, on fabricated trajectories, with no system running — the
   shape `test_harness_stale_split_teeth` established. Feed the settle predicate a square
   wave and it must reject; a monotone decay and it must accept; and — the detection gap —
   a slow cycle whose windowed VARIANCE is small but whose `TV` is large, which a variance
   test passes and `R` must fail. That last case is to `R` what `0 < aged <= bound` was to
   the summed stale bound: the region where the weaker predicate cannot see the defect at
   all.

### Assertable versus reportable

**Assertable — structural, timing-free, bit-exact, in the synthetic rig:**

- *Draw-invariance under the correct ordering.* Same offered sequence, two different draw
  streams, `ingress` tapping before the gate: the published pressure trajectories are
  **bit-identical, epoch for epoch**. This is the dynamics form of the tap-placement claim
  and it is exact rather than statistical, because with the tap first the counter is a
  function of the offered sequence alone. It is the strongest thing in this document: it
  proves the internal loop is open, over time, without any band, threshold or tolerance.
- *Draw-DEPENDENCE under the inverted ordering,* asserted as a failure the way
  `test_harness_tap_after_gate` asserts a broken identity. Same two draw streams, tap after
  the gate: the trajectories must DIFFER. If they ever stop differing, the invariance
  assertion above has lost its teeth.
- *Non-vacuity:* the two draw streams must produce different `dropped_shed` counts in the
  correct arm too. Otherwise "identical trajectories" would be satisfied by draws that
  never mattered anywhere.
- *Reproducibility:* the whole synthetic run repeated yields a bit-identical trajectory,
  which is `enf_determinism` and `obs_determinism` extended over a closed loop.
- *The slow ramp holds pressure at `ADCE_PRESSURE_MIN`* over the steady portion of a
  geometric ramp with `g` well below `g*`, while offered volume grows 100×, and the
  admitted count stays under `rate·elapsed + capacity`. Assertable because §2B derives it —
  not a band, a fixed point of the recurrence.
- *The fast ramp lifts pressure above `ADCE_PRESSURE_MIN`* for `g` above `g*`, which is what
  keeps the previous assertion from being vacuous.
- *The settle metric's teeth* on fabricated sequences.

**Reportable only:**

- Whether the real-time arm settles. Real threads, real clock, real RNG, and a settle band
  with no derivation behind it.
- The responsive-client configurations of §2E, in full. The client model is invented, so
  any threshold on its output measures the model.
- Every step-response number: overshoot, epochs-to-recover, resonant period from the sweep.

**So: oscillation can be MEASURED and cannot be ASSERTED, and this is the answer rather
than a hedge.** The reason is specific, not a general reluctance. A settle band `B` can
only come from observing runs and picking a number that those runs satisfy — which is
tuning an assertion to its own evidence, and this project has recorded four separate
occasions where a single observation was quoted as a rate and the arithmetic built on it
did not survive contact: `0.067`/`ADCE_REPEAT=10`, the `0.65%` suppression rate, the
`recovered=261` bound, and the `~87%` detection figure derived from the first. A closed-loop
band would be the fifth.

**What the measurement would have to show before anyone claims the design settles:**

1. A settle criterion — `W`, `B`, `D` — **fixed and written down before the runs**, derived
   from the tuning constants rather than from observed trajectories. `W >= 4/a` has such a
   derivation. `B` and `D` do not yet, and finding one is the precondition, not an
   afterthought.
2. That criterion met across the whole of §2's pattern family, including C and the full
   period sweep of D — not on one shape.
3. **The inverted arm failing the same criterion on at least one pattern.** Without this the
   criterion has no demonstrated sensitivity and a green result is unfalsifiable. This is
   the load-bearing requirement and the one most likely to be skipped.
4. The responsive-client arms reported alongside, with their client models named, so a
   reader can see that the claim is scoped to the internal loop.

Until 1 and 3 are satisfied, the correct statement is "the internal loop is open, proven
exactly; the external loop is unmeasured" — which is more than this repository can say
today and less than "the design settles".

## 6. File layout

| Path | Contents | Why there |
| --- | --- | --- |
| `test/t_adce_loop.c` | the synthetic rig, the settle metrics, their teeth, and the reported real-time arm | a new file, not an addition to `t_adce_harness.c`, which is already 1500 lines and is about a call site rather than about dynamics |
| `docs/closed-loop-harness.md` | this document | |
| `test/t_adce_platform.c` | six forwarder declarations and six runner-table rows | the single runner table; required by the ran-tests guard in `scripts/verify.sh` |

Nothing in `src/` or `include/` changes. Every injection point this rig needs already
exists and already has a stated reason for existing.

Cases are `static int test_<name>(void)` with one external forwarder each, per the
convention `test/t_adce_observe.c` established.

## 7. The first assertions, in dependency order

1. `loop_synthetic_determinism` — the rig run twice yields a bit-identical pressure
   trajectory. Everything below is meaningless if this fails, and it fails loudly if any
   hidden clock or unseeded stream reached the rig.
2. `loop_draw_invariance` — correct ordering, one offered sequence, two draw streams:
   identical trajectories, and different `dropped_shed` totals. The internal loop is open.
3. `loop_inverted_draw_dependence` — the teeth: same experiment, `ingress_inverted`,
   trajectories must differ. Passes by observing a divergence, as
   `test_harness_tap_after_gate` passes by observing a violated identity — and, like it,
   fenced on stderr, since it will print a deliberate divergence.
4. `loop_settle_metrics_teeth` — `PP`/`TV`/`DC`/`R` on fabricated sequences: square wave
   rejected, monotone decay accepted, and the slow small-variance cycle rejected. No system
   runs; nothing times anything.
5. `loop_ramp_below_threshold` — geometric ramp at `g = 0.02` (`z = 1.73`, derived):
   pressure stays at `ADCE_PRESSURE_MIN` across the steady portion while offered volume
   grows 100×, and admitted stays under the bucket ceiling. §1.2, as a number.
6. `loop_ramp_above_threshold` — the same rig at `g = 0.2` (`z = 4.06`, derived): pressure
   leaves `ADCE_PRESSURE_MIN`. Keeps 5 from being vacuous.

Case 5 needs one derivation not yet done: where the steady portion begins. The initial
condition `mu = var = 0` gives `z = 7.18` at epoch 0 — near saturation — and that transient
decays as `(1-a)^k`, which is still 0.14 after the 100 warmup epochs. **The prime length
must be derived from that decay, not chosen by looking at a trajectory**, or case 5 becomes
the fifth tuned number. The form is: prime until the transient's contribution to `z` is
below `z_lo` minus the ramp's own steady `z`, with margin.

## Open decisions, deliberately not defaulted

1. **`B` and `D`, the settle band and direction-change bound.** No derivation exists. §5
   makes this the precondition for any settling claim rather than a parameter to pick.
2. **The client model for §2E.** Fixed in-flight and backoff have opposite signs; choosing
   one decides the external loop's stability by assumption.
3. **Whether the real-time arm belongs in the per-edit gate at all.** The synthetic rig is
   sub-second and deterministic; the real-time arm is neither, and `t_adce_latency.c` is the
   precedent for measuring-without-asserting in the gate.
4. **The contended cost of `adce_obs_tap`.** Unmeasured, and it is the term that decides
   whether the real-time arm's offered rate is achievable. It is a `t_adce_latency.c`
   question, not this rig's.
5. **Whether `N` may be retuned past 125.** §2B shows that is where a geometric ramp gains
   the ability to saturate the squash. Not a decision to take implicitly.
