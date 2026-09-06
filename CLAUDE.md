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
it. The Enforcement Plane has NO out-of-line surface, and its absence from `src/` is a
resolved decision rather than a gap: `adce_enf_admit` is inline, so it draws from the
CALLING translation unit's `adce_rng_tls`, and an out-of-line `adce_enf_thread_init` would
warm a different stream than the one the gate draws from — silently. The reasoning is in
`docs/enforcement-plane.md` §6; a reader looking for `src/adce_enforce.c` should stop
looking.

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
- `include/adce_obs_thread.h` / `src/adce_obs_thread.c` — the SCHEDULING half of the
  Observation Plane, and optional by construction: a consumer that owns its own cadence
  drives `adce_obs_epoch_close` directly and never links this TU. It makes not one
  statistical decision. It lives in the library rather than in a test so that consumers do
  not each reimplement epoch cadence and writer ownership, which would retire the
  `T <= ADCE_ADVICE_TIMEOUT_NS / 2` `_Static_assert` that currently locks the invariant.
- `include/adce_enforce.h` — the ENTIRE Enforcement Plane, inline, with no `.c` file: the
  outcome enum, `adce_enf_ctx_t`, the deployment tuning block, `adce_enf_should_shed`,
  `adce_enf_classify_stale`, `adce_enf_decide`, `adce_enf_admit`, `adce_enf_thread_init`.
  Integer arithmetic only — no `double` and no `adce_rng_next_unit` — per the lane
  convention in `adce_platform.h`. Every function that must be reachable from a test takes
  its nondeterminism as a PARAMETER: `now_ns` and the RNG `draw`. That is structural, not
  stylistic, and the reason is in the plane doc: `adce_rng_tls` is `static _Thread_local`
  at file scope, so a test TU cannot observe or seed the stream an enforcement TU draws
  from.
- `test/t_adce_enforce.c` — Enforcement Plane unit cases: the exhaustive shed mapping,
  monotonicity, the read clamp, the stale fallback and its three routes, cold start, the
  bucket ceiling, and determinism.
- `test/t_adce_harness.c` — the INTEGRATION harness, and the only file here that tests a
  CALL ORDER rather than a function. It builds an instrumented ingress site in both the
  correct and the inverted orderings, because a harness that only ever runs the correct one
  proves nothing about ordering; it also owns the concurrent case, the fail-closed stale
  posture with its own gated epoch closer, and the deterministic teeth for the stale route
  split.
- `test/t_adce_latency.c` — per-arrival cost, MEASURED and never asserted. A latency
  threshold on a shared runner is a flake generator and this case runs in the per-edit
  gate, so its only assertions are structural: that each fixture drove the outcome it
  claims.
- `docs/` — three design documents, written before the code they describe and cited
  throughout the decisions below: `observation-plane.md`, `enforcement-plane.md`, and
  `closed-loop-harness.md`. The third describes a harness that does not exist yet; see the
  unverified list.

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
- Landing on `main` is ENFORCED, not conventional. A GitHub ruleset (`main`, id 22339037,
  `enforcement: active`) requires a pull request and three passing status checks —
  `sanitizers (ubuntu-24.04)`, `sanitizers (ubuntu-24.04-arm)` and `shipping-target`, which
  is every job `.github/workflows/verify.yml` defines, so nothing in the workflow is
  optional. `bypass_actors` is EMPTY: the repository owner is not exempt. It also forbids
  force-pushes (`non_fast_forward`) and branch deletion. A direct push to `main` is rejected
  with GH013.

  Three details recorded because getting any of them wrong would misdescribe the control.
  It is a RULESET, not classic branch protection: `GET /repos/.../branches/main/protection`
  returns 404 "Branch not protected", and reading that as "unprotected" would be exactly
  backwards — the rules live under `/repos/.../rules/branches/main`, and GH013 is the
  ruleset violation code rather than the classic one. `required_approving_review_count` is
  **0**, so what is enforced is the PR PATH and the checks, not human review; a solo author
  can still self-merge, and this control is not a reviewer. And
  `strict_required_status_checks_policy` is false, so a PR may merge on checks that ran
  against a branch behind `main`.

  This did not replace a written convention — there was none. Every change since #1 has
  landed by PR as an unwritten practice; what changed is that the practice is now
  enforced by the server instead of by whoever is at the keyboard.

  **`gh pr checks` shows FOUR green checks and only three of them are required.** The
  fourth is `CodeRabbit`, and on every PR in this repository it reports
  `Review skipped: manual review required for this OSS repository` — it passes green having
  reviewed nothing. Verified against the ruleset rather than against the PR page: the
  required contexts are exactly `sanitizers (ubuntu-24.04)`,
  `sanitizers (ubuntu-24.04-arm)` and `shipping-target`. Recorded because a reader counting
  green ticks would conclude the bar is four checks including a review, and the actual bar
  is three checks and no review at all — `required_approving_review_count` is 0, as above.
  A skipped review that renders as a pass is the same defect class as a silently-skipping
  profile, which this project has ruled worse than one that does not exist; it is not this
  repository's to fix, so it is documented instead.
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
- **The z-score detector is a FAST-TRANSIENT detector only, and the token bucket is the
  sole defence against sustained or slowly-growing load.** This is a statement about what
  the Enforcement Plane actually contains, and it is stronger than the qualitative version
  in `docs/enforcement-plane.md` §1.2. Two results from `docs/closed-loop-harness.md`
  compose into it:

  A sustained STEP becomes invisible. The EWMA re-baselines onto the new rate within
  roughly N epochs, `d` returns to zero, and pressure returns to `ADCE_PRESSURE_MIN` — so a
  permanent tenfold overload alarms briefly and is then, correctly by the statistic's own
  definition, no longer an anomaly.

  A geometric RAMP need not alarm at all. On a ramp the EWMA reaches a steady solution in
  which `z` is constant and INDEPENDENT of the absolute rate, so the detector is
  scale-invariant and volume climbs without bound while `z` sits still. Solving that fixed
  point at N = 100 puts the alarm threshold at `g* ≈ 8.98%` growth per epoch: anything
  doubling more slowly than every ~8.1 epochs — **~81 ms** — passes the detector entirely,
  at any amplitude, forever.

  Neither is a defect and neither is fixable by tuning; both follow from `pressure` being a
  z-score, which is a RELATIVE measure. What follows for the design is the load-bearing
  part: outside a window of roughly N epochs after a fast change, `ADCE_ENF_RATE_Q16_PER_NS`
  and `ADCE_ENF_CAPACITY_Q16` are the only thing standing between the system and unbounded
  volume. §1.2's insistence that the bucket's rate must NOT be a function of pressure is
  therefore not a defensive nicety — it is the whole of the protection in the regime that
  matters most.

- **`ADCE_OBS_WINDOW_N` must stay below 125, and no test enforces it.** A live constraint on
  a tuning constant that reads as free.

  `sup z` over all geometric ramps is `1/sqrt((1-alpha)*alpha)`, which is 7.178 at N = 100 —
  BELOW `ADCE_OBS_Z_HI` of 8. So at the current tuning no exponential ramp at any growth
  rate can saturate the squash; the steepest conceivable one caps pressure at 0.836 of
  maximum. That bound rises with N and crosses `z_hi` at **N = 125**, past which a
  sufficiently steep ramp reaches full containment. Raising N would therefore change a
  qualitative property of the system, not merely its smoothing.

  The exact crossover depends on which variance recurrence is used, so it was read out of
  the code rather than assumed. `src/adce_observe.c` computes
  `var = (1-alpha) * (var + alpha*d*d)`, with the `(1-alpha)` multiplying the whole bracket;
  that gives `sup z = 1/sqrt((1-alpha)*alpha)` and a crossover at 125. The other common form,
  `var = (1-alpha)*var + alpha*d*d`, gives `1/sqrt(alpha)` and a crossover at exactly 127.
  **125 is the number for this codebase**; 127 belongs to a recurrence it does not use, and
  quoting it would be off by two in the permissive direction.

  ENFORCED, as of the commit that added this sentence. `adce_observe.h` carries a fourth
  `_Static_assert` beside the cadence and `z_hi > z_lo` ones, in pure integer arithmetic
  because `_Static_assert` cannot evaluate a floating expression — the same constraint that
  makes `ADCE_OBS_Z_HI_INT` exist. Substituting `alpha = 2/(N+1)` into `sup z < z_hi`
  clears every denominator to `2 * z_hi^2 * (N-1) > (N+1)^2`, which at `z_hi = 8` reads
  `128(N-1) > (N+1)^2`: 15744 > 15625 at N = 124 and 15872 > 15876 at N = 125. It is
  written against `ADCE_OBS_Z_HI_INT` rather than a literal 128, so retuning z_hi moves the
  bound with it.

  Proved to have teeth rather than assumed to: in a scratch copy at N = 125 the strict
  profile fails at that assert with its own message and the compiler prints
  `expression evaluates to '15872 > 15876'`; at N = 124 it compiles clean. The assert also
  rejects N = 1, which is the quadratic's other root and is correct — alpha is 1 there, the
  EWMA has no memory, and sup z is unbounded.

- Still unverified, in descending order of how much each would change a decision. The
  order changed on 2026-09-05, and again on 2026-09-06 after PRs #10-#14 landed the
  closed-loop harness; the reasons are stated per entry rather than left implicit.

  **What changed on 2026-09-06, before the entries.** The list was last written at
  `4b83276` (#9). Its entry (1) said `test/t_adce_loop.c` does not exist and that
  closed-loop evidence is ZERO in either direction. That file is on `main` at **2008
  lines** with **twelve** registered cases, and the statement is now false. Stale in the
  UNDER-claiming direction is still stale: it understates what is covered and so misdirects
  the next reader, which is the same defect as overstating it. What follows was read out of
  the merged tests, not out of any summary.

  | case | asserts | explicitly declines |
  |---|---|---|
  | `loop_synthetic_determinism` | same seed twice: bit-identical trajectory, tapped, shed, admitted, and the whole per-arrival verdict checksum; trajectory non-constant | — |
  | `loop_draw_invariance` | two DIFFERENT draw streams, tap before gate: pressure trajectory bit-identical epoch for epoch over 400 epochs; tapped equals the offered total exactly; verdict checksums DIFFER | asserting different `dropped_shed` totals — the witness `closed-loop-harness.md` §5 originally named. Measured to collide: two seeds both shed exactly 24075 of 720000 |
  | `loop_inverted_draw_dependence` | inverted ordering: trajectories DIFFER; tapped differs and is strictly below the offered total | — |
  | `loop_settle_metrics_teeth` | exact integer `PP`/`TV`/`DC`/`R` on fabricated sequences | any settle predicate. `B` and `D` are underived, so the case asserts metric VALUES, never a verdict |
  | `loop_step_response_report` | structural only: publication count, `post.pp > 0`, transient end bracketed, post-transient window `PP==R==DC==0` | every step-response number |
  | `loop_ramp_fixed_point_report` | `rc == 0` and nothing else | every measured `z`, both steady-state comparisons, the whole epoch-by-epoch table |
  | `loop_ramp_below_threshold` | pressure pinned at `ADCE_PRESSURE_MIN` across the derived window AND offered volume grew >= 100x; liveness via publications and `epoch_id` | the bucket-ceiling half — see below |
  | `loop_ramp_above_threshold` | pressure strictly above MIN across the window, and strictly BELOW `ADCE_PRESSURE_MAX` | — |
  | `loop_ramp_teeth` | the floor predicate rejects a ramp above g\*; the lifted predicate rejects one below; a FLAT load passes the floor predicate and is rejected only by the growth half | — |
  | `loop_bucket_closed_form_report` | structural only: `shed == 0`, `admitted + limit == arrivals` | both candidate equalities, both error figures, the residual |
  | `loop_bucket_conservation` | `K*A == C + R*span - R*delta_0 - tau_final` on a synthetic and a real-clock arm, gated on `gaps_over_capacity == 0`; premise asserted via `shed == 0`, `stale == 0`, `limit > 0` | the floor form `A == floor((C+R*span)/K)`, REPORTED beside it and never asserted |
  | `loop_bucket_identity_teeth` | the identity predicate rejects one admission over, one under, the over-admission shape, and — same numbers, stall flag set — a stall | — |

  **Two declines are load-bearing and must not be read as gaps.** #12 narrowed case 5 to
  the Observation-plane half because a geometric ramp CANNOT drive the per-arrival path:
  the g = 0.02 ramp offers 5.24e14 arrivals, ~30 days at the ~5 ns gate cost, so the counter
  is injected and the gate is not in the loop. Asserting a ceiling from a fixture that
  cannot run the gate would be fitting the evidence to the assertion. And #13 reports the
  floor form without asserting it because it needs `L + tau_final < K` and `tau_final` is
  effectively uniform over `[0,K)`: it mismatched 5 of 400 real-clock runs where the
  conservation form mismatched zero. A reported number and an asserted one are different
  claims and the entry keeps them apart.

  **(1) The contended cost of `adce_obs_tap`.** Promoted from inside the old (3) to the
  top. Verified before promoting: `adce_obs_tap` appears NOWHERE in `test/t_adce_latency.c`,
  and that file creates no threads at all, so the contended cost has zero evidence rather
  than weak evidence. It is a relaxed `fetch_add` on ONE cache line shared by every ingress
  thread, so each increment needs exclusive ownership of that line and pays a cross-core
  transfer under contention — structurally the one per-arrival term that does not scale.

  It ranks first because it is the term that decides whether any offered rate derived from
  `gate + clock` is achievable, and `closed-loop-harness.md` §4 already publishes a bound —
  27-59 M arrivals/s per thread — that it explicitly flags as an upper bound the tap may not
  permit. Running it can invalidate a published number by an order of magnitude. Nothing
  else on the list has that reach.

  **(2) The aggregate ceiling under real concurrency, as a two-sided identity.** Runnable,
  untested, and the next task. `loop_bucket_conservation` proves
  `K*A == C + R*span - R*delta_0 - tau_final` single-threaded on both clocks; the shipped
  configuration is per-thread buckets whose aggregate ceiling is `threads * rate`, which the
  deployment-tuning comment in `adce_enforce.h` warns is misread as a global ceiling.
  `test_harness_concurrent` covers that configuration only ONE-SIDEDLY
  (`admitted <= rate*elapsed + capacity`).

  That one-sidedness is a measured blind spot, not a theoretical one. Two scratch mutations
  in #14: advancing `last_refill_ns` on admission only over-admits 65% (13806 against 8368)
  and is caught by four tests; refilling at half rate UNDER-admits 26% (6232 against 8368)
  and is caught by **exactly one test in the suite**, the new single-threaded identity.
  Every pre-existing ceiling check is one-sided and an under-admitting bucket moves away
  from the bound, so all of them pass. A gate silently throttling a quarter more than the
  deployment configured is an availability defect that was invisible here until #14, and it
  is still invisible in the concurrent configuration.

  It ranks below (1) rather than above it because each thread's bucket is private and
  non-atomic, so contention does not change the arithmetic and the extension is more likely
  to CONFIRM than to surprise. Probable confirmation ranks below a measurement that can
  overturn a published bound. What blocks it is small and additive: `first_gap_ns` and the
  per-thread `t_start`/`t_last` are not recorded in `t_adce_harness.c`, and that file has a
  documented history of accounting identities being perturbed by instrumentation — the
  read-only-observer argument from #6 applies and must be stated rather than assumed, since
  that file already distinguishes observers from drains.

  **(3) The Darwin half of `adce_platform_get_entropy`** — the `getentropy` chunking loop —
  has no automated coverage at all. Re-checked: `.github/workflows/verify.yml` runs
  `ubuntu-24.04` and `ubuntu-24.04-arm` only, so CI takes the `getrandom` branch and no job
  runs macOS, which is the platform the per-edit gate runs on. It stays above the two
  runnable entries below because it is the only entry where a whole code path is unexecuted
  by any automated gate.

  **(4) The seqlock retry path has never been timed.** The other half of the old (3), left
  behind when the tap was promoted out of it. §5 of `docs/enforcement-plane.md` measures the
  gate on both architectures per outcome, but every figure comes from a fixture with no
  concurrent publication, so `adce_epoch_read` never retried and §2's estimate of those odds
  remains analytic. Below (3) because the path is executed under
  `test_harness_concurrent` — the measurements there record nonzero `torn` reads — it is
  only its COST that is unmeasured.

  **(5) GCC's TSan runs nowhere**; the GCC profile above is ASan+UBSan only, deliberately,
  so every race result in this project is Clang's. Last among the runnable entries, and the
  position is the list's own principle applied rather than a change of taste.

  The demotion rests on a verified fact. GCC does not implement its own race detector, it
  VENDORS LLVM's — checked rather than assumed, against the same `gcc:14` image the gate
  uses. `gcc -print-file-name=libtsan.so` is built from
  `/usr/src/gcc/libsanitizer/tsan/tsan_rtl*.cpp` and `libsanitizer/sanitizer_common/*`,
  which are LLVM compiler-rt's own filenames compiled in verbatim as source paths, and the
  runtime announces `Running under ThreadSanitizer v3` — the same version string LLVM's
  compiler-rt emits. Same shadow memory, same happens-before engine, same report path.

  So running it would add a second FRONT END over near-identical detection, not an
  independent instrument, and the marginal evidence is a GCC codegen difference rather than
  a second opinion on the memory model. Worth having, worth little. The sharper point is
  that it could not have caught the only timing bug this project has ever found: run
  33748342781 was a phase-accounting race, which is a LOGICAL race, and no ThreadSanitizer
  of any vendor can see one — it appeared in the strict `-O2` profile, and ASan and TSan
  dilate execution enough to mask that class.

  **(6) The settle band, and it is BLOCKED rather than unattempted.** Last, because the
  list ranks by how much RUNNING an entry would change a decision and there is nothing to
  run: no experiment resolves this one. That is the entry's content, not an excuse appended
  to it.

  `DC` is unusable as specified, and this was measured rather than argued. A full-scale
  square wave and a 1-LSB dither produce **identical `DC` (198) and identical `R`
  (199.0000)**, differing only in `PP`, by 65536x. Both metrics are amplitude-blind by
  construction — `R` because it is a ratio, `DC` because it counts signs — so neither can be
  given a deadband from its own definition. The only quantity that separates a full-scale
  limit cycle from quantization dither is `PP`, and a threshold on `PP` IS the settle band
  `B`. **The deadband question reduces to `B` rather than being a second open number**, and
  `B` has no derivation. No threshold was invented; `DC` is reported unusable and
  `loop_settle_metrics_teeth` asserts metric values only.

  `R` is separately compromised in a way that has the same root. It deflates when a window
  mixes a one-time transient with a persistent cycle, because `PP` is set by the transient
  while `TV` is shared: a step to 0.8 with a 0.01 dither reports `R = 4.68` and an implied
  period of 171 epochs against a true period of 2, and at amplitude 87 of 65536 the full
  window reports `R = 1.4937` — which no threshold admitting a settling trajectory can
  separate from the monotone decay's exact 1.0. A trailing window recovers the true 199, and
  `loop_step_response_report` shows such a window is exactly clear of a real step's
  transient (`PP == 0` after the measured 5-epoch decay). But choosing where to open it is
  again an amplitude judgement.

  What IS settled, and is the reason this entry no longer sits at the top: the INTERNAL
  loop is proven open over time, exactly and without a band — `loop_draw_invariance` gives
  bit-identical trajectories across two draw streams over 400 epochs and
  `loop_inverted_draw_dependence` gives divergence under the inverted ordering. The
  DYNAMICS half of `observation-plane.md` §2 — "limit-cycle oscillation rather than
  settling" — still has no executable evidence in either direction, and the rig cannot
  supply it: it produces no limit cycle to measure, which is why the teeth have to fabricate
  one. Settling remains a statement about the EXTERNAL loop, whose client model this
  repository has no basis for choosing, and §5's four preconditions for claiming it are
  unmet — of which the underived `B` is the first.

  Separately, and by design rather than by omission: the `abort()` in `adce_rng_seed` has
  never executed, and cannot without fault injection.
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
