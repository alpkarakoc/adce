# ADCE — Anomaly Detection & Stochastic Containment Engine

C11 plus one named compiler extension (`__int128`). No external dependencies. Target: lock-free, allocation-free, fail-closed
publication path.

## Commands

- `./scripts/verify.sh` — the gate: strict build + ASan/UBSan + TSan + test run.
  A turn is not finished until all three profiles are green.
- Quick syntax check:
  `cc -std=c11 -O2 -Wall -Wextra -Werror -Iinclude test/t_adce_platform.c -lpthread`

## Layout

The codebase is no longer a single header. `adce_platform.h` remains header-only — every
platform primitive, the Q16 lane, the RNG and the seqlock are `static inline` in it — but
the Observation Plane has an out-of-line producer, so `src/` exists and both gates compile
it.

- `include/adce_platform.h` — header-only platform layer, built on C11 `stdatomic.h`
  acquire/release. No mutexes, no spinlocks, no allocation.
- `include/adce_observe.h` — Observation Plane types, the single tuning-constant block
  (T, N, z_lo, z_hi, alpha, the sigma epsilon, the warmup length), and the inline
  per-arrival tap. Pure functions on the publication path — squash and clamp — are inline
  here so the tests reach them without a running observer. `sigma` comes from libm's
  `sqrt`; both gates link `-lm`, and the fail-closed handling sits in the epsilon floor in
  `adce_obs_epoch_close`, written as `!(sigma >= EPS)` so one comparison covers NaN,
  negative and zero.
- `src/adce_observe.c` — Observation Plane producer, off the arrival path: `adce_obs_init`,
  `adce_obs_claim_writer`, `adce_obs_epoch_close`.
- `test/t_adce_platform.c` — owns `main()` and the single runner table. `t_*` unit tests:
  Q16.16 arithmetic (including overflow), token-bucket clamping, RNG sanity, time
  monotonicity, single-threaded seqlock, and a two-thread stress test.
- `test/t_adce_observe.c` — Observation Plane cases. Each is `static`, so the gate's
  ran-tests guard finds it by source pattern, and each has one external forwarder that the
  runner table in `t_adce_platform.c` registers.

## Locked decisions — do not change without stating a reason

- Shipping target is Linux x86_64. Local development target is the host architecture,
  because ARM64's weak memory model is a stricter test of the seqlock than x86_64's TSO.
  Code that passes on ARM64 passes on x86_64; the reverse does not hold.
- `adce_epoch_state_t`: sequence + pressure (Q16.16) + epoch_id + observed_at_ns,
  padded and aligned to exactly one `ADCE_CACHELINE`, fixed by `_Static_assert`.
  `ADCE_CACHELINE` is 64 on x86_64 and 128 on arm64.
- `_Alignas` is applied to the first member, never to the typedef (C11 §6.7.5p2).
  This broke the first build; keep it that way.
- `adce_epoch_publish` / `adce_epoch_read` / `adce_epoch_is_stale` implement the
  publish/consume + fail-closed watchdog pattern.
- Time source is `CLOCK_MONOTONIC_RAW` only.
- Platform-specific primitives (CPU pause, entropy) live behind `ADCE_*` macros in one
  block at the top of the header. Nothing else in the codebase is arch-conditional.

## Working agreement

- No placeholders. `// TODO`, stub returns, and empty bodies are defects, not drafts.
- Never silence a warning; fix its cause. `-Werror` is not removed and sanitizer
  suppression files are not written.
- Read before you write. Never assume a symbol exists — grep for it.
- Present changes as diffs or complete file overrides.
- If a request violates a locked decision above, halt, state the risk, and propose the
  corrected design before writing code.
- `scripts/verify.sh` and the files under `scripts/hooks/` are the verification gate.
  Never modify them in the same commit as the code they check. A gate change is proposed
  first, with its reason, and lands in its own commit.
- `adce_rng_seed` calls `abort()` when the entropy draw fails. A PRNG seeded from a
  failed or partial draw is predictable and every downstream containment decision
  inherits that, so there is deliberately no degraded seeding path.
- Changing a public function's signature, return contract, or failure behaviour is an
  API decision. Propose it and wait for confirmation; never fold it into a step whose
  stated scope was something else.
- `__int128` / `unsigned __int128` is a deliberate, load-bearing compiler extension, not
  an oversight. `adce_q16_mul`, `adce_q16_div`, and the token bucket all need a width
  above 64 bits, and `ADCE_Q16_MAX` is `INT64_MAX`, so a 16-bit left shift of a full-range
  numerator does not fit in `int64_t`. The dependency is made explicit by the `#error`
  guard at the top of the header. Do not "clean it up" — it cannot be removed without
  narrowing the Q16 lane, which is a separate design decision.
- The shipping target builds and its tests pass under GCC 14 on linux/arm64 and
  linux/amd64 (`scripts/verify-linux-gcc.sh`). GCC's `__int128` pedwarn under `-pedantic`
  is resolved by `__extension__` on the two typedefs, with every use routed through them:
  the diagnostic fires on bare casts too, so annotating declarations alone would not have
  covered it. `-pedantic` is intact.
- Verified on real x86_64: GitHub Actions run 33742877639, green on the first attempt.
  Runner `ubuntu-24.04`, Linux 6.17.0-1022-azure x86_64, 2 vCPU AT THE TIME OF THAT RUN --
  the standard runners were widened to 4 vCPU later the same day, as recorded below; this
  line is the record of one run, not a standing property of the label. All three `verify.sh`
  profiles — strict, ASan+UBSan, TSan — executed natively under `Ubuntu clang version
  18.1.3 (1ubuntu1)`, and `verify-linux-gcc.sh`'s linux/amd64 leg ran natively under
  `gcc (GCC) 14.4.0`. The `ADCE_CACHELINE == 64` branch, `__builtin_ia32_pause` and the
  64-byte `_Static_assert`s have therefore now been RUN under sanitizers on the shipping
  architecture rather than only compiled for it. LeakSanitizer executed for the first time
  in this project's history — `verify.sh` enables it only on Linux, and Darwin has no LSan
  back end — and reported nothing; neither did UBSan or TSan.
- GCC's sanitizers now run too, and agree with Clang's. Run 33746269305: `gcc (GCC) 14.4.0`
  ASan+UBSan on `verify-linux-gcc.sh`'s linux/amd64 leg, native x86_64, all 30 cases green
  and not one diagnostic — no `runtime error`, no `SUMMARY:`, no leak report. That matters
  for two specific places rather than in general: `adce_obs_clamp_record` cites UBSan by
  name as the check that catches its `ADCE_Q16_MIN` negation guard, and the `__int128` Q16
  lane rests on the same check. Both are now two-compiler evidence instead of one
  compiler's opinion. The profile is conditional by construction — amd64 only, and only
  where amd64 is native — and it prints a loud SKIP with its reason everywhere else, so
  "GCC's sanitizers passed" and "GCC's sanitizers were not run here" can never be confused.
- This does NOT retire the arm64 evidence, and reading it that way inverts the argument.
  x86_64 is TSO and is the weaker test; arm64's weak ordering is what can expose a missing
  barrier, which is why the local target is the host architecture. What changed is which
  leg is unattended: arm64 native TSan now runs only on a developer machine, because no CI
  job runs on arm64 hardware at all — the linux/arm64 leg is qemu under an x86_64 host.
- arm64 now runs on CI hardware, not just a laptop. `ubuntu-24.04-arm`, Linux
  6.17.0-1022-azure aarch64, all three `verify.sh` profiles under `Ubuntu clang
  version 18.1.3 (1ubuntu1)`, TSan included and silent. It was 2 vCPU when this was
  written and is 4 vCPU now — GitHub widened the standard runners on 2026-09-03, between
  runs 33748879974 (11:17 UTC, `nproc: 2`) and 33750562868 (11:36 UTC, `nproc: 4`). The
  count is recorded per run rather than as a property of the runner label, because it has
  already changed once underneath a claim that named it. Both runners carry the same clang,
  so a future divergence between them is attributable to the machine rather than to the
  toolchain — which is the only reason the comparison is worth anything.
- What that run actually taught, and what an architecture-only evidence list hides: CORE
  COUNT was the discriminating variable, not architecture. A phase-transition race in the
  harness survived four green runs — the development laptop and three 2-core CI runs — and
  surfaced only when a 2-core runner stretched the window between a store and another
  thread observing it (run 33748342781, `nproc: 2` on both legs). It appeared in the strict
  `-O2` profile, not under a sanitizer, because ASan and TSan dilate execution and mask
  exactly this class, and TSan cannot see a logical race at all. So "ran on arm64 and on
  x86_64" is a weaker statement than it looks.

  The strongest single piece of evidence for that is in the failing run itself and was not
  previously written down: the leg that FAILED was `ubuntu-24.04`, x86_64, while
  `ubuntu-24.04-arm` passed in the same run, on the same commit, at the same core count.
  The weaker memory model is the one that broke. That is as clean a separation of core
  count from architecture as this project is going to get, and it also explains why TSO
  offered no protection — a phase-accounting race is a logical race, so no amount of store
  ordering constrains it. Repeated execution under varied scheduling
  is what finds these, which is why the scheduled run exists and why `ADCE_REPEAT` does.
- The development machine is 8 logical cores, not 4: an Apple M3, 4 performance plus 4
  efficiency. The earlier "4-core laptop" named one cluster and read as the whole machine.
  Neither number is the timing regime, and that is the point worth recording. macOS places
  threads by QoS class and migrates them between clusters; this project sets no QoS, and
  Apple Silicon exposes no CPU affinity to pin with, so how many of the harness's threads
  run simultaneously and on which cluster is neither observable from the test nor stable
  between runs. The two clusters differ in clock and microarchitecture, so the
  store-to-observation window differs by placement. **The effective concurrency of the
  local gate is therefore not known.** That is the reason CI evidence outranks laptop
  evidence here despite the laptop being the faster machine: `nproc` on a runner is a
  uniform hard bound, and the laptop's number is not a bound at all.
- The 2-core hardware that found the race is gone, and this must not be read as if it were
  still there. Both runners report `nproc: 4` as of 2026-09-03 11:36 UTC. The claim "no CI
  job has ever run this code on more than two cores" was true when written and is now
  false; every run from 33750562868 onward is 4-core, including the 30-execution
  `ADCE_REPEAT=10` dispatch 33771710873.

  Does that weaken the nightly? **No — it changes what the nightly hunts, and separately it
  invalidates the arithmetic that chose 10.** Taking those in order.

  The phase race needed a thread descheduled between a store and another thread's
  observation. That happens when runnable threads outnumber cores, so a 2-core runner
  produced it readily. At 4 cores the harness's threads more often run genuinely
  simultaneously, and preemption-induced windows narrow: the specific mechanism that found
  that bug is now LESS likely per execution. But true simultaneity is the stronger test for
  the other class — a missing barrier on weakly-ordered arm64 needs two threads executing
  at the same instant, and heavy preemption on two cores can mask it by serialising them.
  The matrix has traded preemption-window coverage for true-parallelism coverage. Neither
  direction is strictly better and the nightly is worth keeping; what is no longer true is
  that it hunts the same thing it was built to hunt.

  The arithmetic is the concrete loss. `ADCE_REPEAT=10` was derived in 25c0e6b from
  p ~ 0.067 per execution — roughly 15 harness executions on 2-core CI with one failure —
  giving ~87% detection per night. That p was measured on hardware no longer in the matrix,
  there is no equivalent measurement at 4 cores, and the bug is fixed so p cannot be
  re-measured. **Read 10 as a round number until something re-derives it.** The honest
  summary is that this project can no longer reproduce the only timing bug it has ever
  found. Restoring that coverage means constraining parallelism deliberately — running the
  strict profile under `taskset -c 0,1` or an equivalent — rather than relying on the
  runner being small. That is a gate change and belongs in its own commit with its own
  reason; it is not done here.
- `verify-linux-gcc.sh` carries a 2-CPU timing profile: the strict `-O2` binary under
  `docker run --cpuset-cpus=0,1`, run `ADCE_PIN_REPEAT` times (default 5). It is on the
  linux/amd64 leg only and gated by the SAME `platform_is_native` test as the GCC sanitizer
  profile, not a second mechanism. The reasons the two skip differ and are stated
  separately: the sanitizer profile skips under emulation because qemu reports faults that
  are not real, the pinned profile because under emulation the scheduling being sampled is
  qemu's. False and meaningless are different defects. It is NOT in `verify.sh`: that gate
  must stay runnable on the development machine, and macOS exposes no affinity API, so a
  pinned profile there could never execute.
- What that profile is and is not, because overstating it would recreate the problem it
  exists to address. `--cpuset-cpus` confines THIS CONTAINER's threads to two logical CPUs
  of a larger machine — the container's `nproc` reports 2, echoed into the log as evidence.
  It is not a 2-vCPU machine. On a real 2-vCPU runner the kernel, the runner agent and
  every other process contend for the same two CPUs; under cpuset the rest of the system
  still has the other CPUs and is not excluded from ours either, since cpuset confines us
  to 0 and 1 rather than reserving them for us. So it reproduces contention among this
  binary's threads on two CPUs and does not reproduce system-wide CPU scarcity. **It does
  not restore the lost coverage; it produces different coverage.** Which of the two the
  phase race needed is UNKNOWN and is likely to stay that way: there is one observation of
  it (run 33748342781), with no instrumentation of what the scheduler did, and the bug is
  fixed, so no further samples can be drawn. One sample cannot separate the two.
- The re-derivation of `ADCE_REPEAT` was attempted and FAILED, and 10 therefore stays a
  round number. Recorded because a number with no basis that is presented as derived is
  worse than one admitted to be arbitrary.

  Structurally: sizing a repeat count to a target detection probability needs a POINT
  ESTIMATE of the per-execution failure rate. A point estimate can only come from observed
  failures. This suite is green, and a green suite yields only an upper bound — zero
  failures in n executions puts the rate under roughly 3/n at 95% — never the estimate the
  arithmetic requires.

  No assertion in the suite can serve as a probe, and this was checked rather than assumed.
  Every harness assertion is one of two shapes, and neither is sensitive. The deterministic
  ones — `harness_check_gate_split`, the tap-order identity — run against frozen `now_ns`
  fixtures and cannot vary with scheduling at all. The ceiling assertions are one-sided
  (`admitted <= ceiling`), and scheduling pressure moves admitted DOWN, away from the
  threshold rather than toward it. That leaves exactly one statistical assertion, the
  stale-posture shed band, and it is deliberately insensitive: across 252 phase-samples
  from 63 local executions, `live_pm` was 0 every single time and `blind_pm` stayed within
  493–506 against a band of 420–580 and a required separation of 150. Its closest approach
  to any threshold was 73 permille, on a band its own comment describes as over four sigma
  wide. It is built not to flake, which is correct for an assertion and disqualifying for a
  probe.

  The fault-injection calibration WAS run, in a scratch copy rather than as a committed
  defect, and it did not separate the configurations. Method: copy the tree, revert 199c476
  to reinstate the phase race, trim the runner to `harness_stale_posture` alone, build
  strict `-O2` — the profile the original failure occurred in, confirmed from run
  33748342781's log, which died at `harness_delta_stale(ps, phase) == arrivals` under
  `== 1/3 strict build + run ==` — and run it 100 times under each configuration.

  | configuration | cpus | failures | 95% CI on the rate |
  |---|---|---|---|
  | Darwin native | 8 | 0/100 | 0 – 2.95% |
  | Docker linux/arm64, unpinned | 8 | 0/100 | 0 – 2.95% |
  | Docker linux/arm64, `--cpuset-cpus=0,1` | 2 | 0/100 | 0 – 2.95% |

  300 executions of a deliberately racy build, zero reproductions. **Pinning's sensitivity
  to this class is therefore UNKNOWN, not recovered, and the pinned profile must not be
  described as a better hunt than an unpinned one — nothing here shows it is.** It is worth
  keeping for a different reason: it exercises a scheduling regime nothing else in the
  matrix covers, at about 17 s, and breadth of regime is defensible on its own without a
  measured detection rate behind it.

  The more consequential finding is about the original number rather than the new profile.
  0/100 bounds the rate below 2.95%, and the 2-vCPU estimate that `ADCE_REPEAT=10` rests on
  was ONE failure in roughly 15 executions — a 95% interval of 0.34% to 27.9%. Those
  intervals OVERLAP, so this campaign does not even establish that the old runners were more
  sensitive than the configurations above. **The 0.067 was never a measurement; it was one
  observation with a confidence interval spanning two orders of magnitude, and the ~87%
  detection figure never followed from it.** So 10 is not a round number because GitHub
  widened the runners — it was never derivable at all, and the runner change merely removed
  the last reason to believe otherwise. Recorded because the earlier entry blamed the
  hardware, and the hardware was not the problem.

  Caveats that keep this from being stronger than it is: the scratch configurations are
  arm64, containerised on Darwin, while the failing run was native x86_64 on bare CI, so
  architecture, containerisation and host all differ alongside core count. And 100
  executions cannot resolve a sub-1% event. This narrows what is known; it does not close
  it. Reproducing the race at a measurable rate anywhere at all is the missing precondition
  for any repeat count in this project having a basis, and no configuration reached for so
  far provides it.
- Classifying a live-phase stale read needs FOUR fields, not three, and the obvious triple
  misclassifies silently. A proposal to capture `(now_ns, observed_at_ns, publication_count)`
  into a per-thread ring buffer cannot separate the three routes of
  `docs/enforcement-plane.md` 4.1, and the failure is not a gap but a wrong answer.
  `adce_epoch_read` returns 0 BEFORE it writes through any of its out-parameters, so on a
  torn read `adce_enf_decide`'s local `observed_at_ns` keeps its initialiser of 0. The
  classifier then evaluates `now_ns - 0`, which is a full monotonic clock reading, exceeds
  `ADCE_ADVICE_TIMEOUT_NS` by many orders of magnitude, and reads as **aged** -- the one
  route the measurements say never happens. A torn read would be recorded as the thing it
  is not, and `observed_at_ns == 0` cannot rescue it either, because a cold-start epoch is
  genuinely zero with a VALID snapshot.

  Adding `have_snapshot` -- the return value already computed on that path -- makes all three
  exactly separable, with no ambiguity and nothing the gate cannot see:
  `have_snapshot == 0` is torn; otherwise `observed_at_ns > now_ns` is future (the unsigned
  wrap, which is why the comparison must be made before the subtraction); otherwise
  `now_ns - observed_at_ns > ADCE_ADVICE_TIMEOUT_NS` is aged. `publication_count` is not
  needed for the classification at all -- it is the denominator of the bound assertion, a
  separate purpose that the proposal folded into the same tuple.

  LANDED as `adce_enf_classify_stale` in `include/adce_enforce.h`, with the three counters
  `torn_reads` / `future_reads` / `aged_reads` on `adce_enf_ctx_t`. `adce_enf_decide` derives
  the verdict AND the route from one classification rather than keeping a second predicate in
  step, so the split is an identity against `stale_reads` instead of a parallel tally;
  `enf_stale_route_equivalence` pins the verdict to the expression it replaced across the
  timeout boundary and the wrap. Classification runs only on the stale branch, so the admit
  path is untouched and the section 5 latency figures stand. The struct grew by 24 bytes --
  additive, with no signature, return or failure change.
- The instrumentation was never shown to suppress anything, and the ~0.65% rate it was
  chasing has no basis. Recorded because the comparison was run specifically to avoid
  building on the number, and because this is the third time in this project a single
  observation has been quoted as a rate.

  The claim under test was one failure at iteration 154 of a planned 500 -- 1/154, 0.65% --
  against an instrumented build that went 0/400. Running the UNINSTRUMENTED TSan binary the
  same 400 times on the same machine, runner trimmed to `harness_stale_posture`, gave
  **0 failures**. Both arms are empty, so nothing here distinguishes "the instrumentation
  suppressed the event" from "there was no event at that rate to suppress", and the
  suppression hypothesis is unsupported rather than refuted. It never had much support: 0/400
  under a true rate of 0.65% happens 7.4% of the time, which is not significant at any
  conventional threshold, and the 95% interval on a SINGLE observed failure in 154 runs is
  0.016% to 3.57% -- a factor of 217, the same error shape as the 0.067 above.

  What the run does settle is stronger than the comparison it was asked for, and it came from
  parsing output the harness ALREADY prints rather than from adding instrumentation. Across
  1600 thread-samples the live-phase stale count never exceeded 10 against an assertion bound
  of ~31, giving `P(X >= 32)` near 6e-20 per thread-sample under a Poisson fit. A 0.65%
  per-run failure would need that tail to be about 2.8e16 times heavier. **So the live-phase
  bound assertion was never a plausible source of the observed failure**, and whatever failed
  at iteration 154 -- a TSan data-race report, a different assertion, or a different tree --
  was not this. Chasing it with a ring buffer behind that assertion would have instrumented
  the wrong thing, however cheap the capture.

  The profile, meanwhile, moves this rate by 10x on its own: strict `-O2` averages 0.34 live
  stale reads per thread, TSan 3.57, measured in `docs/enforcement-plane.md` 4.2. A printf is
  not the largest perturbation in this experiment and never was.
- The RECOVERED phase is checked with the LIVE phase's bound, and that bound's derivation
  does not cover a thaw. `test_harness_stale_posture` calls
  `harness_check_live_phase(ps, HARNESS_PH_RECOVERED, "recovered")` -- the same function, so
  the same `delta_stale <= publications_bound` of ~31. There is no separate argument for the
  recovered phase; the justification comment is written entirely about steady publication and
  is reused across the thaw unexamined.

  Two mechanisms are RULED OUT from the code, recorded so neither is chased again. Back-to-back
  publications from a catch-up close cannot occur here: the closer advances
  `deadline_ns = now_ns + T` on every fire INCLUDING while frozen, the close is a single `if`
  rather than a catch-up loop, and `adce_obs_epoch_close` publishes at most once per call. And
  a publication cannot silently fail after warmup -- the sigma floor clamps sigma, it never
  skips the publish.

  What the derivation misses is that the bound counts STRADDLES while an aged read is not a
  straddle. Torn and future need a concurrent publication and are publication-bounded; aged
  needs only a frozen epoch and is ARRIVAL-bounded. `HARNESS_STALE_BATCH` is 256, so one batch
  landing in a still-aged window contributes ~256 against a bound of ~31. A reported
  `recovered=261` on one site against `recovered=1` on another has exactly that shape, and the
  per-site asymmetry rules out a global stall. **This is a diagnosis, not a reproduction**, and
  the distinction is the point: the route split now printed by the harness is what would settle
  it, because a nonzero AGED term in the recovered phase is the signature and torn/future are
  not.

  RUN, and it did not reproduce: 400 TSan executions, 0 failures, recovered stale peaking at 11
  against the bound of 31 and **aged zero across all 1600 thread-samples**. The predicted
  signature never appeared. Budget not extended. So the recovered bound is now the FOURTH
  number in this project whose supporting event cannot be reproduced -- and unlike the earlier
  three, the reading of WHY the bound is unsound does not depend on reproducing it, because it
  follows from the derivation covering straddles only. Do not quote 261 as a rate; it is one
  observation, the same error shape as 0.067 and 0.65%.

  SPLIT LANDED. `harness_check_live_phase` now asserts `torn + future <= publications_bound`
  and `aged == 0` separately, and because the function is shared this applies to LIVE and
  RECOVERED alike -- deliberately, since the scaling argument turns on whether a route needs a
  concurrent publication and not on which phase is being measured. The blind phases assert the
  mirror (`aged == stale`, `torn == future == 0`), which is what stops `aged == 0` from being
  vacuous: it drives the same counter to the full arrival count.

  `aged == 0` is the assertion `verify.sh`'s comment warns about when it declines to pin to one
  core -- a starved closer is a host artefact, and unattributable reds are worse than a weaker
  hunt. What retires that objection is ATTRIBUTION, not a change of opinion: the failure prints
  `aged=N` beside torn and future, naming the cause. If this ever goes red on a loaded runner,
  read the split before assuming a defect.

  Teeth proved both ways. A closer stalled for twice the timeout inside a live phase (scratch
  copy, harness code only) fires it with `aged=40704 torn=0 future=0`. But that case would have
  breached the OLD summed bound too, so it does not justify the split on its own;
  `harness_stale_split_teeth` covers the part that does, deterministically and without timing:
  `aged=5` with `stale=5` against a bound of 31 passes the old predicate and fails the new one.
  The region `0 < aged <= publications_bound` is the whole detection gap.

  The split does NOT transfer wholesale to `test_harness_concurrent`, and the measurement was
  taken before asserting rather than after. `aged` there is 2972672 of 3870720 taps -- 76.8% --
  and it is WARMUP, not closer starvation: 100 warmup epochs publish nothing, so `observed_at_ns`
  stays 0 and every arrival is cold-start aged with a VALID snapshot. Warmup is 1 s of a ~1.3 s
  run, 76.9%; the count is the warmup window to rounding. The two tests differ in whether warmup
  is inside the measured window -- `harness_stale_posture` measures a post-warmup DELTA and lets
  `HARNESS_PH_PRIME` absorb it -- not in whether the closer can starve. `aged == 0` there would
  fail every run for a reason that is the design working, so it was not asserted.

  What did land is the publication-scaled half in a TIGHTER form than stale_posture's, because
  the true publication count is available at that point instead of having to be derived from a
  window duration: `torn + future <= publications + 1` per thread. Measured 4 aggregate under
  strict, 22 under TSan, bound 32. And `aged > 0` replaces a bare `stale > 0`, naming the
  cold-start posture as the thing that ran.

  RESOLVED by matching the window rather than weakening the bound. `harness_concurrent` now takes
  one `harness_snap_t` per site at the confirmed first publication -- each ingress thread
  snapshotting its OWN site, since main reading a running thread's plain counters is a race no
  seqlock covers -- and asserts the live-phase shape over the delta: `torn + future <=
  publications + 1` and `aged == 0`. Measured: aged 0 on every thread and every profile, torn 0
  under strict and 1-4 under TSan against a bound of 32.

  The snapshot cannot perturb `total_tapped == arrivals_closed + discarded + residual`, and the
  argument is structural, not empirical: that identity is about where ARRIVALS go, and a snapshot
  moves no arrival -- it only READS the counters and writes storage on neither side. The contrast
  worth keeping is `g_st_thaw_discarded`, which DID need a term on both sides because it is a
  drain that removes arrivals. Read-only observers need no matching term; drains do.

  Also fixed: the teeth banner now goes to STDERR, the same stream as the expected failures it
  explains, with a BEGIN/END fence. stdout is block-buffered through the gate's `tee` while
  stderr is unbuffered, so a printf banner and an fprintf failure did NOT arrive in written
  order -- four expected FAIL lines surfaced at the top of the gate output with the explanation
  forty lines below. One stream cannot reorder against itself.
- Still unverified, in descending order of how much each would change a decision.
  (1) GCC's TSan runs nowhere; the GCC profile above is ASan+UBSan only, deliberately, so
  every race result in this project is Clang's. (2) The Darwin half of
  `adce_platform_get_entropy` — the `getentropy` chunking loop — has no automated coverage
  at all: CI is Linux-only and takes the `getrandom` branch, so that code runs only on the
  development machine. No CI job runs macOS, which is the platform the per-edit gate runs
  on. (3) Closed-loop behaviour — oscillation, settling, limit cycles — has no load model
  and no evidence in either direction. (4) Per-arrival latency under CONTENTION.
  §5 of `docs/enforcement-plane.md` now carries the measurement — both architectures, per
  outcome, with the method — so the gate's cost is no longer an open question. What is
  still open is that every one of those figures comes from a fixture with no concurrent
  publication: `adce_epoch_read` never retried, so the seqlock's retry path has never been
  timed and §2's estimate of those odds remains analytic. Separately, and by
  design rather than by omission: the `abort()` in `adce_rng_seed` has never executed, and
  cannot without fault injection.
- `adce_q16_div` with a zero divisor saturates toward the numerator's sign: negative
  numerator yields `ADCE_Q16_MIN`, zero or positive yields `ADCE_Q16_MAX`. A collapsed
  divisor therefore reads as maximal pressure downstream, never as zero. `0 / 0` is
  `ADCE_Q16_MAX` by this rule. This is a fail-closed contract; changing it is an API
  decision.
- A FUTURE `observed_at_ns` reads as maximally stale, and the unsigned wrap in
  `adce_epoch_is_stale` is what produces it. A reader that cannot order the publication it
  read against the clock it read has no coherent view of time and must not act on that
  publication, so the wrap is load-bearing rather than an oversight: replacing it with a
  signed difference, a saturating guard, or an `observed_at_ns > now_ns` branch returning 0
  turns the case fail-OPEN. A torn `adce_epoch_read` counts as stale for the same reason —
  no snapshot means no advice. Both are reachable without any fault and both require a
  concurrent publication, so nonzero `stale_reads` on a healthy system is expected. Full
  statement in `adce_platform.h` above `adce_epoch_is_stale` and in
  `docs/enforcement-plane.md` §4.1, with the measurements. This is a fail-closed contract;
  changing it is an API decision.
- Rounding is toward negative infinity across the whole Q16 lane. `adce_q16_to_int`
  floors via its arithmetic right shift, and `adce_q16_div` floors by stepping the
  truncated quotient down when the remainder is non-zero and the operand signs differ.
  Truncation toward zero was the earlier behaviour and left a dead band two LSBs wide
  around zero, where the quantization step doubles — a pressure signal driving threshold
  crossings behaves differently there than anywhere else in its range. Flooring is
  uniform. This is a contract; changing it, or changing one function without the other,
  is an API decision.
- Long-running measurement loops are launched with `nohup ... & disown` and write progress to
  a file under the scratchpad. A scheduled check-in or a new turn can kill a job still
  attached to the shell; this cost three restarts of one reproduction run. Poll the progress
  file, never the process.
- Long-running measurement loops are launched with `nohup ... & disown` and write progress to
  a file under the scratchpad. A scheduled check-in or a new turn can kill a job still
  attached to the shell; this cost three restarts of one reproduction run. Poll the progress
  file, never the process.
