# Closed-loop harness — design

Status: **all of §7 implemented in `test/t_adce_loop.c`.** The synthetic rig, the settle
metrics and their teeth, the §2B confrontation, and both ramp cases with their
counterfactuals run in the per-edit gate. Case 5 is narrower than first specified — its
bucket half is not assertable from a ramp and §7 says why. What remains unbuilt is the
external loop: the responsive-client patterns of §2E and the real-time arm, both of which
§5 keeps reportable rather than assertable. This document exists because
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
closing.

That sentence used to continue "**nothing in this repository has ever run the detector and
the actuator against each other over time**", and `test/t_adce_loop.c` has made it false.
What is now run is the INTERNAL loop, exactly: the published pressure trajectory is
bit-identical across two draw streams under the correct ordering and diverges under the
inverted one, over 400 model epochs. That is §1's internal loop shown open over time, with
no band and no tolerance.

The dynamics half of §2 — "limit-cycle oscillation rather than settling" — still has no
executable evidence in either direction, and the rig does not supply it: the rig produces
no limit cycle to measure, which is why §5's teeth have to fabricate one. Settling remains
a statement about the EXTERNAL loop, and §5's preconditions for claiming it are unmet.

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

This is a DIFFERENT ERROR CLASS from the four corrections recorded in CLAUDE.md, and
conflating them would misplace where the discipline needs to be applied. `0.067`, the
`0.65%`, `recovered=261` and the `~87%` were single observations used as measurements —
a methodology fault, where the defect is in what was treated as evidence and no amount of
rechecking the arithmetic would have found it. This was a unit conversion inside a
derivation that is otherwise correct and independently recomputable: the fixed point, `g*`
and `sup z` all stand unchanged, and anyone can recompute the column from the formula now
printed beside it. The first class is caught by refusing to quote an observation as a rate;
the second by showing the conversion. Both are worth catching, and the remedies do not
transfer.

Two derived numbers fall out, neither of which is written down anywhere in this repository:

- **`g* ≈ 8.98% per epoch` is the alarm threshold for exponential growth.** Any ramp
  doubling more slowly than every 8.06 epochs — 81 ms — passes the detector entirely, at any
  amplitude, forever. That is §1.2's claim as a number.
- **`sup z = 1/sqrt((1-a)a) = 7.178 < z_hi = 8`, so a geometric ramp can never saturate the
  squash at all.** The steepest conceivable exponential ramp caps pressure at 0.836 of
  maximum. And this is a property of the current tuning, not of the shape: `sup z` crosses
  `z_hi` at **N = 125**. A future retune of N past 125 silently changes whether a ramp can
  ever reach full containment. It is invisible at the call site and no runtime test would
  catch it, so it is pinned by a `_Static_assert` in `include/adce_observe.h` instead:
  `2 * z_hi^2 * (N-1) > (N+1)^2`, the same inequality with every denominator cleared, which
  holds at N = 124 and fails at N = 125.

  The crossover depends on which variance recurrence is used, so it was read out of the
  code rather than assumed. `src/adce_observe.c` computes `var = (1-a)*(var + a*d*d)`, with
  the `(1-a)` outside the bracket, giving `sup z = 1/sqrt((1-a)a)` and 125. The other
  common form, `var = (1-a)*var + a*d*d`, gives `1/sqrt(a)` and exactly 127. 125 is this
  codebase's number; 127 would be off by two in the permissive direction.

**CONFRONTED WITH THE CODE, AND CONFIRMED.** `loop_ramp_fixed_point_report` in
`test/t_adce_loop.c` drives the real `adce_obs_epoch_close` down a geometric ramp past a
derived prime and reports the measured steady `z`. It asserts nothing.

| g | predicted `1/sqrt(v)` | measured | rel err vs 2B | rel err vs the OTHER recurrence |
|---|---|---|---|---|
| 0.02 | 1.72660898 | 1.72660898 | **1.570e-12** | 1.005e-02 |
| 0.20 | 4.05595335 | 4.05595335 | **5.562e-14** | 1.005e-02 |

Seven to nine orders of magnitude inside the `eps = 1e-5` the prime was derived for. The
result is not a shared fixed point reached from two lucky points: the worst deviation from
the constant across the WHOLE steady window is 2.1e-10 at g = 0.02 and 1.5e-10 at g = 0.20.
`z` really is constant on a geometric ramp, in the implementation and not only in the
algebra.

It also DISCRIMINATES rather than merely agreeing. The two candidate variance recurrences
differ by exactly `1/sqrt(1-a) = 1.010051` at every `g`, and the measurement sits 1.005e-2
away from the other form — so this codebase uses `var = (1-a)*(var + a*d*d)`, `sup z` is
`1/sqrt((1-a)a)`, and **the committed `_Static_assert` pinning `N` below 125 is pinned to
the right number.** 127 would have been wrong by two in the permissive direction, and that
is now measured rather than read.

Two structural facts came out of building the measurement, and both constrain cases 5 and 6:

- **A geometric ramp cannot use the per-arrival path.** The g = 0.02 ramp over 700 epochs
  offers 5.24e14 arrivals — about 30 DAYS at the ~5 ns gate cost of `enforcement-plane.md`
  §5. The rig must inject the epoch counter directly, which is exactly what `n` taps leave
  behind (`adce_obs_tap` is a `fetch_add`, `adce_obs_counter_take` an exchange) but is not
  the ingress path. Any ramp case is an OBSERVATION-plane case; it cannot exercise the gate.
- **At large `g` the cold-start prime is unreachable in `uint64`.** 576 epochs at g = 0.20
  needs `1.2^576 = 4.06e45` arrivals, 26 orders of magnitude past `UINT64_MAX`, even
  starting from one arrival.

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
- *Non-vacuity:* the two draw streams must produce a different per-arrival VERDICT
  SEQUENCE in the correct arm too. Otherwise "identical trajectories" would be satisfied by
  draws that never mattered anywhere.

  This bullet originally named `dropped_shed`, and the implementation refuted it. That
  count is a scalar sum of ~38,000 Bernoulli trials, so two streams collide on it whenever
  their sums happen to coincide -- and the FIRST seed pair tried did exactly that: seeds
  `0xA5A5A5A5` and `0x5EED1234` both shed exactly 24,075 of 720,000 arrivals while their
  verdicts differed throughout, which failed the assertion on a rig that was working
  correctly. Across 24 seeds the totals ran 23,801..24,282 with a standard deviation of
  111, putting the collision probability near `1/(2*sd*sqrt(pi))` ~ 0.25%. **That figure is
  ANALYTIC, from the observed spread. The one collision is a single observation and is not
  quoted as a rate** -- the error this project has recorded four times.

  A checksum over the verdict sequence has no such failure mode: it differs unless the two
  streams produced the same verdict for every arrival, which is the exact statement
  non-vacuity exists to exclude. The totals are still printed; they are just not asserted.
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
| `test/t_adce_platform.c` | one forwarder declaration and one runner-table row per case | the single runner table; required by the ran-tests guard in `scripts/verify.sh` |

Nothing in `src/` or `include/` changes. Every injection point this rig needs already
exists and already has a stated reason for existing.

Cases are `static int test_<name>(void)` with one external forwarder each, per the
convention `test/t_adce_observe.c` established.

## 7. The first assertions, in dependency order

**Status: 1-4 LANDED in `test/t_adce_loop.c`. 5 and 6 are not implemented** -- they rest on
the section 2B fixed point, and that derivation has not been confronted with the code;
cases 1-4 are decidable from the offered sequence and the published integers alone and so
do not inherit that exposure. A fifth case, `loop_step_response_report`, is registered
beside them: it is NOT case 5 below, it asserts no band, and it exists to supply the half of
case 4 that a fabricated sequence cannot -- that a window opened after the measured end of a
transient is exactly clear of it on a real step.

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
5. `loop_ramp_below_threshold` — geometric ramp at `g = 0.02` (`z = 1.7266`, measured):
   pressure stays at `ADCE_PRESSURE_MIN` across the steady portion while offered volume
   grows 100×. §1.2, as a number. **LANDED, and NARROWED — see below.**
6. `loop_ramp_above_threshold` — the same rig at `g = 0.2` (`z = 4.0560`, measured):
   pressure leaves `ADCE_PRESSURE_MIN`. Keeps 5 from being vacuous. **LANDED**, with the
   added bound that pressure stays strictly BELOW `ADCE_PRESSURE_MAX`, which is §2B's
   "no ramp at any rate saturates the squash" as a runtime fact rather than an algebraic
   one.
7. `loop_ramp_teeth` — the counterfactuals for both. **LANDED.**

### The bucket half of case 5 is deliberately not written

Case 5 above once also asserted "and admitted stays under the bucket ceiling". It does not,
and the reason is structural rather than an omission.

A geometric ramp **cannot drive the per-arrival path**: the g = 0.02 ramp offers 5.24e14
arrivals, about 30 days at the ~5 ns gate cost. The counter must be injected, so **the gate
is not in the loop** for any ramp case. Writing the ceiling half anyway would mean inventing
a fixture whose only purpose is to make the claim assertable — fitting the evidence to the
assertion, which is the failure mode this document exists to avoid.

**The narrowing costs nothing, because the two halves belong to two different mechanisms.**
CLAUDE.md already records the split: the z-score detector is a FAST-TRANSIENT detector only,
and the token bucket is the sole defence against sustained or slowly-growing load. What a
ramp demonstrates is *Observation going blind* — which is precisely case 5's pressure half,
and precisely §1.2's claim. The ceiling claim belongs to the bucket, and it is already
evidenced by the fixture that can actually run the gate: `test_harness_concurrent` asserts
`admitted <= rate*elapsed + capacity` per thread under live four-thread load, with measured
admitted landing within a handful of arrivals of the ceiling. Not unevidenced — evidenced
elsewhere, by the right instrument.

### The bucket's own fixture — confronted, not yet asserted

`loop_bucket_closed_form_report` is the fixture the narrowing above said the ceiling claim
needs. It reports; it asserts nothing beyond the structural guard that pressure really was
pinned and the bucket really was the only limiter.

**The derivation.** With pressure pinned at `ADCE_PRESSURE_MIN` and the epoch kept fresh,
`adce_enf_should_shed` is `(draw >> 48) < 0` — false for every draw — so every arrival
reaches stage two. Summing `adce_enf_decide`'s bucket update over `M` arrivals telescopes to

    K*A = C + R*(t_last - t_start) - L - tau_final

with `L` the tokens the cap discarded. The elapsed terms telescope *only* because
`last_refill_ns` is advanced on every arrival that reaches stage two, **dropped ones
included** — that is the load-bearing detail, not an incidental one.

**The residual has a derivation.** `L` is not zero and cannot be: `adce_enf_thread_init`
starts the bucket FULL, so the first arrival refills into a bucket already at capacity and
discards exactly `R*delta_0`. Measured, the identity closes with `L = R*delta_0` exactly in
both arms — the initial clamp and nothing else. A later clamp needs a gap long enough to
refill from starved back to capacity, `C/R = 38.3 ms`; none occurred.

**Both arms are exact.**

| arm | span | admitted | predicted | relative error |
|---|---|---|---|---|
| synthetic, exact 1 µs spacing | 40.000 ms | 8368 | 8368 | **0.000e+00** |
| real `adce_now_ns`, 2e6 arrivals | ~32 ms | 7504 | 7504 | **0.000e+00** |

The real-clock arm survives clock granularity because the derivation never assumes anything
about *where* the timestamps come from — it is pure conservation, and a repeated or coarse
reading contributes `delta = 0` without breaking the telescoping.

**Which equality to assert, and which is a trap.** The obvious form
`A == floor((C + R*span)/K)` requires `L + tau_final < K`, and `tau_final` is effectively
uniform over `[0,K)` across runs, so it fails whenever `tau_final` lands in `[K-L, K)` —
probability about `L/K`. Over 400 real-clock runs it mismatched **5 times**. (One campaign,
consistent in order of magnitude with `L/K`; not quoted as a rate.) The conservation form

    K*A == C + R*(t_last - t_start) - R*delta_0 - tau_final

never divides, so it has no leftover condition at all, and every term is directly
observable. Over the same 400 runs it mismatched **zero** times. **That is the equality a
later step should assert.** The floor form would have been an underived number arrived at by
a different route.

### The conservation equality, asserted

`loop_bucket_conservation` asserts `K*A == C + R*span - R*delta_0 - tau_final` on both a
synthetic and a real-clock arm, gated on the measured precondition `gaps_over_capacity == 0`.

**A stall fails the case, it does not skip it.** `L = R*delta_0` holds only while no gap
refills the bucket from starved back to capacity (`C/R = 38.3 ms`). If one did, the identity
is not the right equation — but returning "pass" would be worse, because the case would
report OK having checked nothing. This project already ruled that a silently-skipping profile
is worse than one that does not exist; the same holds for a silently-skipping assertion.
So a stall is a RED that names the stall, and `gaps_over_capacity` is printed beside the
identity. That is the same argument that let `aged == 0` be asserted: what makes a red
acceptable on a loaded runner is ATTRIBUTION, not a lower bar.

Observed headroom is large: under TSan, the slowest profile, the real arm's max inter-arrival
gap was **14,459 ns against the 38,347,922 ns threshold** — a factor of 2600.

**Why an equality rather than another inequality.** Two mutations were run in scratch copies:

| mutation | effect | caught by |
|---|---|---|
| `last_refill_ns` advanced only on admission | 13806 admitted vs 8368 — **65% over**-admission | `loop_bucket_conservation`, `enf_bucket_ceiling`, `harness_concurrent`, `harness_stale_posture` |
| refill at half rate (`elapsed/2`) | 6232 admitted vs 8368 — **26% under**-admission | **`loop_bucket_conservation` only** |

Every pre-existing ceiling check is one-sided (`admitted <= rate*elapsed + capacity`), and an
under-admitting bucket moves *away* from that bound, so all of them pass. A gate silently
throttling a quarter more traffic than the deployment configured is an availability defect
that nothing in this repository could previously see. That asymmetry is the argument for the
two-sided form, and it was measured rather than assumed.

The floor form is reported beside the identity and never asserted, with its rejection reason
inline so nobody reaches for it later.

### What the teeth had to cover, and what the obvious mutation misses

The natural counterfactual for case 5 is a ramp above `g*`, and it is necessary: it breaks
the pressure half, proving the predicate can tell 8.98%/epoch from 20%/epoch. It is not
sufficient.

**A FLAT load also holds pressure at `ADCE_PRESSURE_MIN`.** With `g = 0` the deviation goes
to zero, sigma falls to the epsilon floor, `z` goes to zero, and pressure sits at the floor
with nothing growing anywhere — so case 5's pressure half is satisfied by a rig that has
stopped. Measured: a flat load at the same `n0` and epoch count PASSES `loop_ramp_check_floor`
outright.

So case 5's content is not "pressure at MIN" but "pressure at MIN **while offered volume
grew**", and the growth is therefore ASSERTED rather than described. `loop_ramp_teeth`
carries both mutations: `g` above `g*` rejected by the floor predicate, and `g = 0` rejected
by the growth predicate while passing the floor one. That second region — where the pressure
predicate cannot see the defect at all — is the same shape as `0 < aged <= bound` was for the
summed stale bound.

Case 5 needed one derivation not yet done: where the steady portion begins. **That
derivation is now DONE**, in closed form and without inspecting a trajectory, and is
implemented in `loop_ramp_fixed_point_report`.

The initial condition `mu = var = 0` gives `d_0 = r_0` and `var_0 = (1-a)a r_0^2`, so
`z_0 = 1/sqrt((1-a)a) = 7.17775745` exactly — the same `sup z` that bounds the ramp family,
which is a coincidence worth noticing rather than a derivation. Two bounds follow:

- **Cold start:** the absolute transient decays as `(1-a)^k`, so `k = ln(eps)/ln(1-a)`. At
  `a = 2/101`, `ln(1-a) = -0.02000067`, giving **576 epochs at `eps = 1e-5`** — which
  resolves `z` to five significant figures, the precision this document quotes its
  predictions to. `eps` is a measurement-precision requirement with a stated consequence,
  not a tuning choice.
- **Ramp-specific**, needed because the bound above is unreachable at large `g`: `z` is a
  ratio, so what must decay is the RELATIVE transient, and the signal grows underneath it —
  the `mu` error decays as `((1-a)/(1+g))^k` relative to `r_k`, the `var` error faster.
  `k = ln(eps / |z_0/z_inf - 1|) / ln((1-a)/(1+g))`, every term closed form. 319 epochs at
  g = 0.02, 56 at g = 0.20.

**The prefactor is load-bearing and dropping it makes the bound too short.** The naive form
`ln(eps)/ln((1-a)/(1+g))` gives 290 epochs at g = 0.02 where the recurrence actually needs
306. And the approach is NOT monotone: `z_k` crosses its own limit at epoch 24 (g = 0.02)
and epoch 10 (g = 0.20) and then undershoots by 6-7%, so a prime chosen by watching a
trajectory for "close enough" could stop at the crossing, land on the right value by
accident, and be wrong a few epochs later. That is the concrete reason this had to be
derived.

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
