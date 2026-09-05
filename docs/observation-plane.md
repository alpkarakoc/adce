# Observation Plane — design

Status: proposed. No implementation exists. Written before any producer code so the
semantics published into `adce_epoch_state_t` are pinned first.

## Contradictions found against the repository at time of writing

- **"The tap precedes the gate" was not a locked decision.** Every occurrence of "gate" in
  CLAUDE.md refers to `scripts/verify.sh`, the verification gate. There was no enforcement
  gate, no tap, and no Ingest Plane definition in the repository. The three plane names
  existed only in comments in `include/adce_platform.h`. This document is where the
  principle is written down.
- **`adce_platform.h` line 199 conflicts with line 427.** The first says Q16.16 is for the
  Ingest and Enforcement planes only and that the Observation Plane is where floating point
  is permitted; the second says the Observation Plane writes `pressure`, which is Q16.16.
  Resolved in section 3: the Q16 conversion *is* the plane boundary.
- **Narrowing the Q16 lane would NOT remove the `__int128` dependency.** CLAUDE.md names
  three consumers. The token bucket multiplies two `uint64_t` values (`rate * elapsed`) and
  `adce_q16_mul` multiplies two full-range `int64_t`; both need 128 bits regardless of how
  narrow `pressure` is. Narrowing would remove the extension from `adce_q16_div` alone.
  Lane width and the extension are separable questions.

## 1. What `pressure` is

Dimensionless normalized containment score. Q16.16, contract range `[0, ADCE_Q16_ONE]`
(raw `[0, 65536]`). Monotone: higher means more containment. `0` = pass everything;
`1.0` = most conservative posture.

Not a rate and not a raw ratio. Both are unbounded, and unbounded advice forces every
consumer to invent its own saturation point, placing a statistical decision in the plane
that has no statistical context. Normalizing in Observation defines the saturation curve
once, in the code that owns the distribution.

**Negative is invalid and must never be published**, but Enforcement must still handle one,
and must clamp it to *maximal* pressure rather than zero. A negative can only come from a
bug, a misconfigured squash, or a corrupted read. Reading "less than no anomaly" as
"nothing to do" fails open exactly when the system is misbehaving.

**Cold start needs no new mechanism.** A zero-initialized `adce_epoch_state_t` has
`pressure == 0`, which alone would fail open. It is safe only because `observed_at_ns` is
also 0, so `adce_epoch_is_stale(0, now)` is true and Enforcement takes its conservative
posture. The staleness watchdog, not the pressure value, is what makes the
pre-first-publish state safe. This is load-bearing and must not be optimized away.

**On `ADCE_Q16_MAX == INT64_MAX`.** Correct for the *type*, wrong as the *pressure
contract* — different things. The `adce_q16_*` functions are general-purpose fixed-point
primitives; narrowing their saturation bound to suit one consumer is backwards coupling.
Introduce a separate publication contract `ADCE_PRESSURE_MAX == ADCE_Q16_ONE`, clamped at
publish and re-clamped at read. This composes with the locked divide-by-zero rule: a
collapsed divisor yields `ADCE_Q16_MAX`, far outside `[0,1]`, which clamps to maximal
pressure — fail-closed, and distinguishable from a legitimate 1.0.

The clamp inside `adce_q16_div` remains effectively dead on the pressure path. That is not
an argument for narrowing: the clamp exists for the general primitive, where
`ADCE_Q16_MIN / -1` reaches it.

## 2. Tap placement

    arrival ──▶ [TAP]  observation: fetch_add(&epoch_arrivals, 1, relaxed)
                  │
                  ▼
               [GATE]  enforcement: read epoch → stale? → drop / admit
                  │
        ┌─────────┴─────────┐
      drop                admit ──▶ downstream work

The tap sits on the arrival path, strictly before the gate, and counts every arrival
including ones the gate is about to drop.

**What breaks if reversed.** Tapping after the gate closes an actuation-feedback loop:
enforcement drops traffic, so observation sees fewer arrivals, so the measured rate falls,
so pressure falls, so enforcement drops less, so the rate rises. The controller measures
its own output instead of its input. Two consequences: limit-cycle oscillation around the
threshold rather than settling; and, worse, an attacker who pushes just past the threshold
gets the system to self-soothe — the anomaly vanishes from the measurement *because*
containment engaged, so containment disengages and the attack is admitted. The signal must
be open-loop with respect to the actuator.

The tap runs per arrival on the ingress thread, so it must be O(1), allocation-free,
non-blocking, and inline. One relaxed `atomic_fetch_add` is the whole tap.

Admit/drop outcomes may be counted separately for telemetry but must never feed the
pressure statistic; folding them back re-creates the loop by another route.

## 3. Detection mechanism

Exponentially-weighted z-score of arrival rate.

Per epoch k of fixed duration T: snapshot-and-reset the arrival counter for `n_k`, then
`rate_k = n_k / T`.

    d     = rate_k - mu_{k-1}              deviation against the PRIOR mean
    mu_k  = mu_{k-1} + alpha * d
    var_k = (1 - alpha) * (var_{k-1} + alpha * d * d)
    z_k   = d / max(sqrt(var_k), epsilon)
    press = clamp((z_k - z_lo) / (z_hi - z_lo), 0, 1)

Deviation is taken against `mu_{k-1}`, not `mu_k`, so the current sample cannot mask itself.

Window: `alpha = 2 / (N + 1)`. Recommended T = 10 ms, N = 100, giving a ~1 s effective
window.

**Why EWMA rather than a sliding window of samples.** This is the direct answer to carrying
window state across epochs without allocation: with EWMA the state *is* the window — two
scalars fixed at compile time, O(1) in time and space, no ring buffer, nothing proportional
to N. A fixed ring buffer can also be allocation-free but costs N * sizeof(sample) of state
and re-raises eviction and cold-start handling for no accuracy gain at this job.

Squash is piecewise-linear (`z_lo ~ 3`, `z_hi ~ 8`), not logistic: exactly representable in
Q16, no transcendental on the path, and its saturation points are auditable numbers rather
than curve parameters.

**Plane boundary.** `mu`, `var`, and `z` are `double`, owned exclusively by the observer
thread and never shared. The only value crossing into Q16 is the final squashed score,
converted and clamped at the publish call. `adce_platform.h` line 199 therefore holds as a
statement about internal arithmetic, and the Q16 conversion is the boundary. The header
comment should say so.

**Epsilon floor on sigma.** Without it, perfectly steady traffic drives sigma toward zero
and any deviation reads as unbounded z. The locked divide-by-zero rule turns that into
maximal pressure — arguably correct, but hair-trigger after a quiet period. Tie epsilon to
a configured minimum rate.

## 4. Single writer, enforced rather than assumed

One dedicated observer thread, created once at init, publishing once per epoch
(T = 10 ms). Not a signal handler, which cannot safely do this work. Never piggybacked on
an ingress thread: that would make writer identity depend on which ingress thread happened
to run, which is precisely the assumption a seqlock cannot survive.

Enforcement: an `_Atomic int` claim flag on the observer context, CAS'd 0 -> 1 at init. A
second claimant fails at startup, loudly, rather than corrupting the seqlock at runtime. A
`pthread_self()` equality check against the recorded owner on each publish is affordable
because publish is once per epoch, not per event; keep it always on rather than debug-only.

The arrival counter is many-writer (every ingress thread) and is `_Atomic uint64_t` with
relaxed `fetch_add`. Only the epoch state is single-writer. Conflating the two is the
likely bug here.

Cadence invariant: `T <= ADCE_ADVICE_TIMEOUT_NS / 2`. At T = 10 ms against the 50 ms
timeout there is 5x margin, tolerating four consecutive missed publishes before the
watchdog trips. Both are compile-time constants, so this is a `_Static_assert`.

## 5. Testable now vs. needs a harness

Testable without a running system, all pure and deterministic:

- EWMA update against precomputed mean/variance for a fixed rate sequence.
- Squash: monotone, saturating, `z_lo -> 0`, `z_hi -> 1`, never escapes `[0,1]`.
- Publication clamp: every input, including `ADCE_Q16_MAX` from a divide-by-zero, lands in
  `[0, ADCE_Q16_ONE]`; negatives clamp to maximal.
- Warmup: nothing published before `N_min` epochs.
- Writer claim: the second claim fails.
- `T <= ADVICE_TIMEOUT / 2` static assert.
- Determinism: identical input sequence yields identical pressure sequence.

Requires an integration harness, proposed as its own step:

- That the tap actually precedes the gate. This is a property of the call site, not of any
  function; no unit test can assert it.
- Oscillation and limit-cycle behaviour under closed-loop traffic. **Designed in
  `docs/closed-loop-harness.md`; still unmeasured.** The ordering claim in section 2
  above has executable evidence (`test_harness_tap_before_gate` and its inverted
  counterpart); the DYNAMICS half of that section -- "limit-cycle oscillation rather
  than settling" -- has none in either direction, and that document says which part
  of it can be asserted and which can only be measured.
- Timer jitter and epoch drift under load; whether the watchdog trips under real scheduling.
- Fail-closed behaviour when the observer thread dies mid-flight.

**Testability constraint flowing back into the design:** `adce_now_ns()` is not injectable.
The epoch-close function must therefore take `observed_at_ns` as a parameter rather than
calling the clock internally, or none of the above is deterministically testable.

## 6. File layout

| Path | Contents | Why there |
| --- | --- | --- |
| `include/adce_observe.h` | types, constants, `adce_obs_tap()` inline | per-arrival hot path; one relaxed `fetch_add`, must inline |
| `src/adce_observe.c` | `adce_obs_init`, `adce_obs_claim_writer`, `adce_obs_epoch_close` | once per epoch, off hot path; out-of-line keeps the header small |
| `test/t_adce_observe.c` | the seven assertions in section 5 | first test file for this plane |
| `include/adce_platform.h` | unchanged | |

Two dependencies, each needing its own commit: `scripts/verify.sh` compiles only
`test/t_adce_platform.c` and would not build `src/` or the new test at all, which is a gate
change proposed separately; and CLAUDE.md's Layout section says "single header", which
stops being true.

## Open decisions, deliberately not defaulted

1. **Cold-start posture.** Fail-closed during warmup means a startup outage; fail-open means
   a bounded blind window. The existing watchdog defaults this to fail-closed. This is an
   availability decision, not a technical one.
2. **T, N, z_lo, z_hi.** Proposed 10 ms / 100 / 3 / 8. Tuning; wrong defaults are worse than
   none.
3. **Rate alone vs. multi-signal.** Arrival rate is one dimension; real anomaly detection
   usually also wants size, error rate, or per-source cardinality. `pressure` is a scalar,
   so multi-signal needs a stated fusion rule.
4. **Whether `ADCE_PRESSURE_MAX` is a hard clamp or a saturating counter.** A clamp discards
   overshoot magnitude, which Enforcement may want to know.
