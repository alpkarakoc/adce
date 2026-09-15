# ADCE — Anomaly Detection & Stochastic Containment Engine

C11 plus one named compiler extension (`__int128`). No external dependencies. Target: lock-free, allocation-free, fail-closed
publication path.

## Commands

- `./scripts/verify.sh` — the gate: strict build + ASan/UBSan + TSan + test run.
  A turn is not finished until all three profiles are green.
- Quick syntax check of ONE translation unit, without linking:
  `cc -std=c11 -O2 -Wall -Wextra -Werror -Iinclude -fsyntax-only test/t_adce_platform.c`
  `-fsyntax-only` is load-bearing, not tidiness. The earlier form omitted it and linked
  `-lpthread`, which cannot succeed: the runner table in that file forwards to cases defined
  in the other five test files, so the link fails on every `adce_t_*` symbol. See the scope-gap
  entry below.

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
  `adce_obs_claim_writer`, `adce_obs_claim_counter`, `adce_obs_drain`,
  `adce_obs_residual`, `adce_obs_epoch_close`.
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
  publish/consume + fail-closed watchdog pattern. Two of the three are still wired into it:
  `adce_epoch_publish` has one shipping call site and `adce_epoch_read` has one.
  `adce_epoch_is_stale` has ZERO and is reached only from tests — see the entry on that, and
  the open API question it carries.
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
  an oversight. **Its scope is narrower than this entry recorded for most of the project's
  life, and the correction is below rather than folded in silently.** Nothing is removed;
  what changes is how many legs the justification actually stands on.

  Three sites use the width, and they are NOT three independent shipping justifications:

  | site | reaches 128-bit | shipping call sites |
  |---|---|---|
  | `adce_q16_mul` | own body | **0** |
  | `adce_q16_div` | own body | **0** |
  | `adce_token_refill` | **own body, directly** | 1 (`adce_enf_decide`) |

  **The token bucket does not reach the width through the Q16 helpers.** Read out of the
  source rather than carried: `adce_token_refill`'s first statement is
  `adce_u128_t added = (adce_u128_t)rate_q16_per_ns * (adce_u128_t)elapsed_ns;`, then the
  sum, then the clamp. It calls neither `adce_q16_mul` nor `adce_q16_div`. So the one
  executed leg is independent of the two that are not executed, and removing either Q16
  helper would not touch it.

  `ADCE_Q16_MAX` is `INT64_MAX`, so a 16-bit left shift of a full-range numerator does not
  fit in `int64_t` — that argument is about `adce_q16_div` specifically, which is the leg
  with no shipping caller. The dependency is made explicit by the `#error` guard at the top
  of the header. Do not "clean it up" — it cannot be removed without narrowing the Q16 lane,
  which is a separate design decision. **That conclusion survives, but on the PUBLISHED LANE
  rather than on three shipping paths**, and the difference matters when someone next weighs
  the extension against portability.
- **The `__int128` re-derivation, measured. At the SHIPPED tuning no execution path can
  reach a 64-bit overflow at all.** The evidence behind the re-scoped locked decision above,
  taken by RUNNING each site against a 64-bit formulation of itself rather than by reasoning
  about widths. Every row below was reproduced on this tree, not carried from the branch
  that first wrote it.

  | site | first divergence from a 64-bit formulation | 64-bit result | shipping callers |
  |---|---|---|---|
  | `adce_q16_mul` | operands of `65536.0` (raw `2^32`) | `0.0` where the true product is `4294967296.0` | 0 |
  | `adce_q16_div` | numerator raw `2^47` (`2147483648.0`), divisor `3.0` | **sign inverted**: `-715827882.67` for `+715827882.67` | 0 |
  | `adce_token_refill` | `elapsed_ns = 2,635,249,153,387,078,803` at `R = 7` | `5` raw where the true value is the full capacity | 1 |

  The refill row was checked on BOTH sides of the boundary, which is what makes it a
  threshold rather than an example: at `floor(2^64/7) = 2,635,249,153,387,078,802` the two
  forms AGREE, and at `floor + 1` they diverge.

  **The token bucket's threshold is 83.51 years of idle time**, which is `floor(2^64 / R)`
  at the shipped `ADCE_ENF_RATE_Q16_PER_NS` of 7, expressed in 365.25-day years.
  `elapsed_ns` is time since that thread's last stage-two arrival, so reaching it means one
  ingress thread taking no admitted traffic for eight decades. It is not reachable by any
  deployment of this code.

  **And the clamp masks even that, almost entirely.** A wrapped product still exceeding
  capacity clamps to capacity either way, so the two forms agree unless the wrap lands BELOW
  `C`. That window is `C / R` wide — **38,347,922 ns, about 38.35 ms, out of every 2.635e18
  ns, which is 1.46e-9 % of the range.** `C / R` is the same quantity this codebase already
  knows as the gap that refills a starved bucket to capacity, printed as `38347922ns` by
  `loop_bucket_conservation`; the two are the same number arrived at from opposite ends.

  So the width at that site is **defensive against retuning and against consumer-supplied
  rates, not against the shipped configuration.** The rate at which it starts to matter is
  `2^64 / T` for an idle window `T`: **584.5 for a year of 365.25 days**, 213,504 for a day,
  5.12e6 for an hour. The year figure carries its convention because it moves with it —
  584.9 at 365 days, 586.6 at 52 weeks — and the branch this entry came from said "about
  586" without one, which is the only figure of the set that did not reproduce. The
  deployment tuning block invites exactly this retuning, which is why the width stays.

  **What this does and does not overturn.** It does NOT overturn the decision: the lane is
  published, `adce_q16_div` is callable by consumers at full range, and at full range the
  64-bit form inverts the sign — stated as a fact rather than as a safety property, though
  it is worth knowing that a negative Q16 reads as MAXIMAL by the clamp rule, so the failure
  would be wrong rather than open. It DOES overturn the entry's weight. "All three need a
  width above 64 bits" is true of the three FUNCTIONS and false as a count of shipping
  justifications, and a justification standing on one of three named legs is weaker than the
  old wording read.

  **THE GATE CANNOT SEE THIS, AND THAT IS THE MEASURED CASE FOR REFINING IT.** `internal-use`
  is now live on `main`, and run against this tree it places all three sites in columns that
  read as healthy:

  | site | `internal-use` column | what the gate concludes | what is true |
  |---|---|---|---|
  | `adce_q16_mul` | **annotated green** | public API with no internal user, by design | no shipping path executes it |
  | `adce_q16_div` | **annotated green** | same | same |
  | `adce_token_refill` | **live** (1 shipping caller, never printed) | wired up | correct |

  The check asks one question — does a call edge exist from shipping code — and answers it
  exactly. It does not and cannot ask whether the justification written ABOUT a function is
  still true, because that is a fact about prose evaluated against the call graph, and the
  gate reads only the call graph. **Two of the three legs the locked decision names sit in
  the green column while contributing nothing to the conclusion.** Nothing went red here;
  nothing should have. This is the boundary of what a zero-caller predicate can reach, found
  by hand, and it is the concrete case for the three-way live / test-only / dead refinement
  rather than an argument for it. That refinement is NOT started here.

  **This is the third instance of one class in as many tasks**, and the class is now worth
  naming as a habit rather than as an incident: `adce_epoch_is_stale`, the `__int128` legs,
  and `adce_rng_next_unit` are all prose that names a real function, describes its behaviour
  correctly, and is wrong about the system because the code takes a different path. The
  common cause is that **a justification is written once, at the moment it is true, and is
  never re-evaluated against the call graph afterwards.** `scripts/check-internal-use.sh`
  catches the zero-caller end of it. It does not catch this one, and nothing does: a leg with
  ONE shipping caller is invisible to a zero-caller check, and two of the three functions the
  Q16 lane annotation names are annotated-green rather than red. Read every named
  justification in this document as dated.

- **THE MERGE IDIOM BROKE WHEN `internal-use` LANDED DELIBERATELY RED, and the cost was
  priced in late.** `gh pr checks <n> && gh pr merge <n>` stops at the first command:
  `gh pr checks` exits non-zero unless EVERY check is green, and `internal-use` is
  permanently red by design, holding two open API questions visible. So the chain silently
  performs no merge.

  The correct form is `gh pr checks <n> --required && gh pr merge <n>`, which narrows the
  criterion to the three ruleset contexts — `sanitizers (ubuntu-24.04)`,
  `sanitizers (ubuntu-24.04-arm)`, `shipping-target`. **Dropping the check from the command
  entirely would remove verification rather than narrow it**, and that is the wrong repair
  for a command that failed by being too strict.

  **The cost, stated because it was not priced when the check was designed.** A permanently
  red advisory check makes "all green" unusable as a merge criterion for as long as it stays
  red — not just for that pull request, for every one after it. The check's own design
  treats the red as the feature, and it is; what was not noticed is that it consumes a
  second thing, the simplest available expression of "is this safe to merge". The bound on
  that cost is the five-pull-request answer window opened at #26's merge: the red is
  supposed to be answered, and when it is, `--required` stops being load-bearing. If the
  window closes with the reds still standing, this stops being a transitional cost and
  becomes a standing one, which is a fact the window's review should weigh.

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
- **The second gate is a CORRECTNESS control for timing-dependent derivations, not only a
  portability control.** Recorded because #16 is the first case where it caught something
  `verify.sh` could not, and the reason generalises beyond that commit.

  What happened: #16 extended the bucket conservation identity to the concurrent case, and
  its first version carried a precondition inherited from the single-threaded rig — that a
  refill can only clamp after an inter-arrival gap reaching `C/R = 38.3 ms`, on the
  reasoning that a starved bucket needs that long to refill to capacity. `verify.sh` was
  GREEN on it across nine executions under `ADCE_REPEAT=3`, all three profiles, and
  `verify-linux-gcc.sh` was green locally too. `shipping-target` on CI failed it:
  `diff -401047`, about six admissions' worth.

  The derivation was simply wrong, and wrong in the permissive direction. A refill clamps
  whenever `tau + R*delta` exceeds `C`, so while the bucket is still near FULL early in a
  run, a gap of only `(C - tau)/R` clamps — 9.4 us at one admission below capacity, not
  38 ms — and the harness sleeps 1 ms between batches. The fix measures the discarded
  amount exactly instead of bounding it, which removed the precondition entirely.

  **What makes this a coverage finding rather than an anecdote is WHY the local gate could
  not see it.** The bug was not a portability defect: the same source, the same compiler
  family, the same architecture. It was reachable only on a host whose thread scheduling
  produced a large enough early gap while the bucket was still near capacity. The
  development machine is an 8-core M3 whose effective concurrency is already recorded above
  as NOT KNOWN; the CI runners are 4 vCPU under a different scheduler and a different load.
  The measured discarded total shows the regime difference directly: 36,169 to 408,345 Q16
  across eleven local executions, against 27,909 to 3,236,938 across fourteen on CI — an
  8x wider upper end.

  So the standing claim that `verify.sh` is the gate and `verify-linux-gcc.sh` is the
  shipping-target check is incomplete. Any assertion whose CORRECTNESS depends on a timing
  regime — every conservation identity in `t_adce_loop.c` and `t_adce_harness.c` does, via
  their clamp and staleness preconditions — is only as sound as the widest set of schedules
  it has been evaluated under. A green local gate on such an assertion is weaker evidence
  than it looks, and the second gate is where that weakness is currently caught. This does
  NOT retire the arm64-versus-x86_64 argument recorded above; it adds a second, independent
  reason the second gate is load-bearing.

- **"Four clamp events per run" is FALSIFIED, and the falsifying observation was already in
  the run that merged #16.** Recorded because the error is this project's own recurring
  one, committed again at one remove.

  #16's commit message says the identity held "always over exactly four clamp events — one
  per thread, the initial full-bucket refill". Four is the structural expectation: each
  thread's bucket starts full, so its first stage-two arrival necessarily clamps, and under
  sustained overload the bucket then drains and does not return to capacity. Eleven local
  executions all showed four. That is a regularity, not a law, and "always" overstates it.

  It is worse than overstated — it is false, and by evidence available at the time. The
  green `shipping-target` run 34053044909 printed clamp counts for fourteen executions:
  **thirteen with four clamps and one with five**, alongside a discarded total of 3,236,938
  Q16, eight times the largest seen locally. Across all twenty-five executions the tally is
  twenty-four fours and one five. The claim was refuted in a log that was read for PASS or
  FAIL and not for its numbers.

  One thing follows that is not about the bucket, and it is the rule below rather than the
  reading failure this entry first blamed. **"The log was read for PASS or FAIL and not for
  its numbers" is the symptom, not the cause**, and stating it as the cause was wrong in a
  way worth correcting: it makes the defence proportional to attention. The claim was about
  a quantity nothing checked, so no amount of care would have kept it true as hosts changed
  — care catches the instance, and only a check catches the class.

  The identity itself never depended on the count: it asserts
  `C + R*span == K*A + L + tau_final` with L MEASURED, so five clamps are as exact as four.
  Nothing about the assertion changes; only what may be said about it does.

- **PRINTED IS NOT CHECKED. Do not state a universal about a quantity that is only
  printed.** The generalisation of the clamp-count finding above, and the rule that replaces
  its first diagnosis.

  A number a test prints has no gate behind it by construction. Nothing fails when it
  changes, so a universal asserted about it — "always", "never", "exactly N" — is
  unfalsifiable by the gate and drifts silently as hosts, core counts and schedulers move
  underneath it. That is precisely how "always over exactly four clamp events" survived
  into a commit message while a run in the same PR printed five.

  **The remedy has exactly two branches and no third.** Either

  1. ASSERT it, at which point the gate is the check and the claim is maintained by the
     thing that would go red; or
  2. say nothing stronger than **a range with its observation count beside it** — "24 of 25
     executions showed four, one showed five", not "always four".

  Anything between the two is a claim maintained by whoever last looked, which is the same
  defect this project already records for a silently-skipping profile: the problem is the
  ABSENCE OF A CHECK, not the absence of vigilance.

  **This does NOT mean assert everything printed, and reading it that way would break the
  design on purpose.** `t_adce_latency.c`, `loop_ramp_fixed_point_report`,
  `loop_bucket_closed_form_report` and `loop_step_response_report` deliberately measure
  without asserting, because a threshold on those numbers would be a band with no
  derivation — the failure mode this list exists to prevent. Reporting a measurement and
  claiming a law about it are different acts, and only the second is forbidden here. A
  report-only case is fully correct while its prose says "measured X across n runs" and
  becomes wrong the moment it says "X always".

  **There is a THIRD branch, and it outranks both when it is available: remove the
  nondeterminism so the quantity becomes assertable.** Branches 1 and 2 take the
  nondeterminism as given — one gates on it, the other declines to generalise past it. The
  third dissolves it. The clamp regime is the worked example: it was entered by scheduling
  luck in one of twenty-five executions, which is why nothing stronger than 24-of-25 could
  be said about it. Where the rig owns model time the same regime can be CONSTRUCTED, and a
  constructed regime is exercised on every run on every host, which moves the quantity from
  branch 2 into branch 1. Reach for this first; fall back to 1, then to 2.

  Applied backwards through this document, the entries that already state a figure with its
  n — 0/100 in the fault-injection table, 493-506 across 252 phase-samples, 5 of 400
  real-clock runs, 24 of 25 clamp counts — are in the second branch and stand. Any bare
  universal about a printed quantity is in neither branch and should be read as unsupported
  until it is one or the other.

- **The printed-universal rule CANNOT be gated, and the audit that establishes that is a
  NEGATIVE result worth recording.** Written down because the natural next move — add a grep
  to the gate — is wrong, and the reason is a measurement rather than an opinion.

  The rule was grepped across the whole of this document on
  `\b(always|never|every|exactly|invariably)\b`, which is the widest pattern that could
  plausibly catch it. **66 lines matched. No violation of the rule as stated was found among
  them** — a claim narrower than the "zero violations" first written here, for the reason in
  fault one below. The breakdown is the finding:

  - structural facts about the code, where the universal is a property of the source and not
    of a measurement (`aligned to exactly one ADCE_CACHELINE`, `every use routed through
    them`, `shared by every ingress thread`);
  - prohibitions and contracts, where the universal is the instruction (`never fold it into
    a step`, `never asserted`, `never links this TU`);
  - figures that already carry their n, which is branch 2 working (0/100, 493-506 across 252
    phase-samples, 0/400, 1600 thread-samples);
  - quantities that ARE asserted, which is branch 1 working (`aged == 0`);
  - and prose about the rule itself.

  **The numerator's provenance, TWICE wrong, and the second correction is the instructive
  one.** 66 replaced a prior expectation of 27. The reason first recorded — "the pattern used
  here is wider than one keyword" — was under-explained; the reason recorded second, that 27
  is `\bnever\b` case-sensitive, reproduces 27 exactly and is nonetheless NOT what was run.
  The actual pipeline was the five-word-plus alternation with `-i`, a second `grep` removing
  lines that already carry their `n`, and `head -30`. On the audited document that pipeline
  has **56 true hits**. Thirty were displayed. Twenty-seven was reported: a miscount of a
  truncated listing.

  **Fault one: the census behind "zero violations" was partial and was read as complete.**
  The audit saw at most 30 of 56 hits and drew a universal over the set. So the near-miss was
  missed for TWO independent reasons, and only the first was recorded above: the pattern could
  not reach that sentence, and the reading had already stopped before the end of what the
  pattern did reach. Either alone is sufficient, which is why fixing the pattern would not
  have rescued the audit. This is the same defect one entry above — a run's log read for PASS
  or FAIL and not for its numbers, refuted by a line inside the very log that was read. A
  partial view read as complete. What survives is narrower than what was claimed: no violation
  was found among the lines actually examined, and the census that would license the word
  "zero" over the document was never completed. The conclusion the entry rests on does not
  need it — the argument against a grep gate is the ratio and the unreachable near-miss, and
  both hold at 56 as they do at 66.

  **Fault two: the provenance was DERIVED from the number rather than obtained.** `\bnever\b`
  case-sensitive was fitted to a single figure, hit it exactly, and was wrong about the
  mechanism — wrong about the pattern, wrong about the `-i`, wrong about the second filter,
  and wrong that 27 was even a hit count rather than a miscounted display. Exact agreement on
  one point carried no information, because many mechanisms pass through one point.

  This is `docs/closed-loop-harness.md` §2B applied backwards. There the two candidate
  variance recurrences were separated by a prediction that DISCRIMINATES: they differ by
  `1/sqrt(1-alpha)` at every `g`, the measurement landed 1.6e-12 from one and 1.005e-2 from
  the other, and the `_Static_assert` pinning `N` below 125 is pinned correctly because of it.
  A prediction that merely agreed would have left both alive. Here the discriminating step was
  cheaper than any measurement: **ask what command was run.** It was available for free and
  was not taken, and a fitted model was published in its place. Prefer the observation that
  separates candidate mechanisms over the one that confirms the mechanism you already have —
  and when the mechanism is somebody's shell history, that observation is a question.

  Two of the three figures re-derive from the document at `9b37397^` — 66 for the bare
  five-word pattern, 27 for the fitted pattern that was never run — and the third does not,
  which is worth stating rather than papering over. 56 is the true hit count of the pipeline
  as its author reports it; the extra alternation term and the exclusion filter are not
  written down precisely enough here to re-run, so 56 is testimony and 66 and 27 are
  measurements. Recording which is which is the whole of the lesson above.

  **The grep fails in BOTH directions, and the UNDER-fire is the heavier fault.** The
  over-fire is the 66 above. The under-fire is the near-miss, and it is the audit earning its
  keep: the `harness_concurrent` entry stated torn reads as "0 under strict and 1-4 under
  TSan" with no execution count. That is not a banned universal — it is a range — but it is
  branch 2 with the n missing. Corrected above by saying so rather than by inventing a count.

  **That sentence contains no word in the pattern family, so no pattern in this family could
  ever have reached it.** This is checked, not inferred: at `9b37397^` the line carrying the
  claim (556) matched the pattern ZERO times, and the line above it (555) matched only on
  `every`, in `aged 0 on every thread and every profile` — the `aged == 0` half, which is
  ASSERTED and therefore a false positive. One line of false positive sitting directly on top
  of one line of invisible true positive, in the same two-line passage. And the miss is not a
  gap in the word list to be patched by adding words: a range with its n missing is defective
  for what it OMITS, and no regex matches an absence.

  **Over-firing is visible; under-firing reads as green.** That asymmetry is why the
  under-fire ranks higher. 66 false positives announce themselves — the gate goes red, someone
  reads it, and the worst case is that the check gets skipped, which is a failure everybody
  can see. A clean run over a document that contains a real violation announces nothing, and
  the reader draws the opposite conclusion from the one the evidence supports. This project
  already ranks that defect class: a silently-skipping profile is worse than one that does not
  exist. A grep gate here would be both at once — loud where it is wrong and silent where it
  is right.

  **The ratio is the argument.** A grep-based gate on this rule would fire 66 times on an
  untouched document and roughly that often after any edit, with a true-positive rate of
  zero and a detection rate, on the single true defect the audit found, of zero as well.
  Tightening the pattern does not rescue it and neither does widening it: what separates a
  violation from a false positive is whether the quantity behind the sentence is asserted
  anywhere, which is a fact about the test suite and not about the sentence, and no regex
  reaches it.

  So this stays a REVIEW RULE. **A review rule is not a control.** It is maintained by
  whoever is reading, which is exactly the property this rule was written to condemn in
  claims — and naming that plainly is the point, because a rule about unenforced claims that
  quietly presents itself as enforced would be the same defect one level up. Its actual
  enforcement is branch 3: every quantity moved out of "printed only" is one fewer sentence
  this rule has to police.

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
  publications + 1` and `aged == 0`. `aged == 0` is ASSERTED, so that half is maintained by the
  gate. The torn figures are not: 0 under strict and 1-4 under TSan against a bound of 32,
  observed over an execution count that was not recorded. Left as the one branch-2 gap the
  printed-universal audit below turned up — a range without its n — rather than back-filled with
  a number invented after the fact.

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

- **Two SCOPE GAPS, recorded as findings. Neither is resolved here, and reading either as
  fixed would repeat the error it describes.**

  **Gap one: the unverified list has gone stale in the UNDER-claiming direction three times,
  and the cause is structural rather than inattentive.**

  | # | what the list said | what was true | recorded |
  |---|---|---|---|
  | 1 | entry (1) at `4b83276`: `test/t_adce_loop.c` does not exist, closed-loop evidence is zero | the file landed across #10-#13 | #15 |
  | 2 | entry (2): the aggregate ceiling under real concurrency is "runnable, untested, and the next task" | landed in #16 | #22 |
  | 3 | the case table: "**2008 lines** with **twelve** registered cases" | 2164 lines, thirteen cases since #20 | this entry |

  The third is still stale as this is written: `loop_bucket_clamp_regime` landed in #20 and is
  absent from the table. Recording it does not fix it — the table is corrected in the same
  commit as this entry, and the correction is not the finding.

  **Three is a pattern and the cause is mechanical.** #16 and #20 each touched test files
  ONLY, with `CLAUDE.md` untouched in both. The commit that moves the boundary and the commit
  that records the move are in different pull requests, so the list is wrong for the whole
  window between them — days, not minutes. No amount of care closes that window, because the
  person who moved the boundary has already merged and gone.

  **The fix is a reading of the "own commit" rule, not an exception to it.** That rule exists
  for GATES: `scripts/verify.sh` and `scripts/hooks/` must not change in the same commit as
  the code they check, because a gate that changes alongside its subject cannot be trusted to
  have judged it. `CLAUDE.md` is not a gate. It checks nothing, fails nothing, and blocks
  nothing; it is the record. So the rule it must satisfy is only that the record be separable
  from the change for review, which a SEPARATE COMMIT INSIDE THE SAME PULL REQUEST already
  satisfies. That preserves the rule's purpose and closes the window.

  Standing instruction, therefore: a pull request that moves a boundary the unverified list
  describes updates the list in its own commit within that same pull request. Not a later one.

  **Gap two: "written is not run" was applied to code paths and never to command snippets.**

  The `Quick syntax check` line in Commands does not link. It compiles
  `test/t_adce_platform.c` alone, and that file's runner table forwards to cases defined in
  the other five test files, so the link fails with undefined symbols for every `adce_t_*`
  forwarder. It was written, committed, and carried through every commit since; it was never
  run.

  The scope gap is the general fact, not the one broken line. This project maintains an entire
  list of code paths that are unexecuted by any gate, and ranks them — the Darwin
  `getentropy` loop sits at (3) precisely because no automated gate executes it. That
  discipline stops at the source tree. A shell command in a documentation file is code that a
  reader will execute, and it is worse than an unexecuted source path in one specific way:
  **a reader runs a snippet BEFORE reading the source, because running it is how they start.**
  An unexecuted branch in `adce_platform.h` misleads nobody until it runs. A broken snippet in
  Commands misleads the first person who tries the project.

  The command is corrected in this commit. **That is the instance and not the class**, which
  is this project's own recurring distinction: care catches the instance, only a check catches
  the class. No check exists here. Every remaining command snippet in this document and in
  `README.md` is in the same unverified position, and this entry does not change that.

- Still unverified, in descending order of how much each would change a decision. The
  order changed on 2026-09-05, and again on 2026-09-06 after PRs #10-#14 landed the
  closed-loop harness; the reasons are stated per entry rather than left implicit.

  **What changed on 2026-09-06, before the entries.** The list was last written at
  `4b83276` (#9). Its entry (1) said `test/t_adce_loop.c` does not exist and that
  closed-loop evidence is ZERO in either direction. That file is on `main` at **2164
  lines** with **thirteen** registered cases, and the statement is now false. (It read
  "2008 lines" and "twelve" until this commit -- stale since #20, which is instance 3 of
  the scope gap recorded above.) Stale in the
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
  | `loop_bucket_clamp_regime` | model time is owned by the rig, so the clamp regime is CONSTRUCTED: `clamp_hits == 200` as an equality, `dropped_limit == 0`, `admitted == arrivals`, and `C + R*span == K*A + L + tau` in a regime where L is large and repeated; two `_Static_assert`s pin the construction to the tuning constants | — |

  **Two declines are load-bearing and must not be read as gaps.** #12 narrowed case 5 to
  the Observation-plane half because a geometric ramp CANNOT drive the per-arrival path:
  the g = 0.02 ramp offers 5.24e14 arrivals, ~30 days at the ~5 ns gate cost, so the counter
  is injected and the gate is not in the loop. Asserting a ceiling from a fixture that
  cannot run the gate would be fitting the evidence to the assertion. And #13 reports the
  floor form without asserting it because it needs `L + tau_final < K` and `tau_final` is
  effectively uniform over `[0,K)`: it mismatched 5 of 400 real-clock runs where the
  conservation form mismatched zero. A reported number and an asserted one are different
  claims and the entry keeps them apart.

  **(1) RETIRED — MEASURED. The contended cost of `adce_obs_tap`.** Kept in place rather
  than deleted, because a retired entry that names its result is what stops the next reader
  re-opening it. The measurement and the verdict on §4's bound are in the entry below. In
  one line: the tap is a relaxed `fetch_add` on ONE shared line, its cost RISES steeply with
  thread count, and §4's 27-59 M arrivals/s per thread is OVERTURNED for multi-threaded
  ingress. What replaced it sits in the same entry, per host and per thread count, with n.

  What remains open from this entry, and it is narrower than what it replaced: the figures
  come from a loop doing nothing but tapping, so they are the worst case for line migration.
  A real ingress site interleaves the gate, the clock and request work between taps, and the
  composition used below assumes the terms ADD. Neither assumption is tested. Measuring a
  full ingress site end to end is the successor question, and it is a smaller one — the
  direction, the mechanism and the order of magnitude are no longer in doubt.

  **(2) RETIRED — LANDED IN #16, AND THE LIST SAID OTHERWISE FOR EIGHT DAYS.** #16 merged
  2026-09-06 and this correction is 2026-09-14; both dates are from `gh`, because the first
  draft of this sentence said "twenty-two days" from memory and was wrong by a factor of
  nearly three. This entry read "Runnable, untested, and the next task" from #9 until this
  commit. It was filed
  as stale under the stopping rule and is corrected here; the staleness is instance FIVE of
  the pattern recorded under gap one, and the first that was knowingly left in place.

  What actually ships, read out of `test/t_adce_harness.c` rather than out of the filing:
  `test_harness_concurrent` asserts `harness_bucket_identity(st, label) == 0` PER THREAD, and
  then the two-sided AGGREGATE identity
  `threads*C + R*sum(span) == sum(K*A) + sum(L) + sum(tau)`, summed per thread rather than
  collapsed onto one global span because each thread contributes its own capacity. The
  one-sided `admitted <= ceiling` check is still there beside them; it was never removed, and
  keeping it is correct — it is a different statement, not a weaker copy of the same one.

  **What the entry said was blocking it was also wrong.** It named `first_gap_ns` and the
  per-thread `t_start`/`t_last` as "not recorded in `t_adce_harness.c`". They are recorded;
  that is what `sum_span`, `sum_L` and `sum_tau` are built from. The read-only-observer
  argument the entry said "must be stated rather than assumed" was stated, in #6's terms, and
  is in the entry on the snapshot above.

  Kept in place rather than deleted, per the convention entry (1) established: a retired entry
  that names its result is what stops the next reader re-opening it. The original text is
  preserved below for what it says about the blind spot, which has NOT changed:

  *(as originally written)* `loop_bucket_conservation` proves
  `K*A == C + R*span - R*delta_0 - tau_final` single-threaded on both clocks; the shipped
  configuration is per-thread buckets whose aggregate ceiling is `threads * rate`, which the
  deployment-tuning comment in `adce_enforce.h` warns is misread as a global ceiling.

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

  **(3) RETIRED — EXECUTED AND GATED. The Darwin half of
  `adce_platform_get_entropy`.** The entry was also WRONG about what the gap was, and the
  correction is the useful part: that arm was never unexecuted. `adce_rng_seed` calls
  `ADCE_GET_ENTROPY` on first use of the RNG, so every local gate execution on Darwin has
  always entered it. What no shipping path reached was the CHUNKING — the only caller asks
  for 16 bytes, 16 <= 256, so the clamp was never taken and the loop never iterated twice,
  in a function whose own comment calls the chunking "mandatory, not defensive".

  Closed by `test_platform_entropy` in `test/t_adce_platform.c`, which is a CONTROL rather
  than a measurement: it runs in all three `verify.sh` profiles, drives six lengths straddling
  the 256-byte limit, and asserts four properties. Details and the mutation proof are in the
  entry below. What remains uncovered and is NOT claimed: the Linux arm's `EINTR` and
  short-read retries, which need a signal to provoke.

  **(4) RETIRED — MEASURED, AND THE PREDICTION REFUTED ON ONE HOST OF THREE.** The seqlock
  spin. In one line: at the shipped cadence it is entered about 3e-9 times per read with four
  readers on the development host, 7.65e-9 on x86_64 CI and NOT ONCE in 2.0e8 reads on arm64
  CI; its cost is bounded by the write window, 1.4 to 10.9 ns depending on host; and the term
  is negligible against the tap on every host measured. The iteration distribution is
  FAT-TAILED on the development host, which refutes what was predicted — but neither CI host
  reproduces that tail, and arm64 CI produced one entry in a billion reads, so the refutation
  does not generalise. Details, the host-by-host tables, and the branch-3 assertion that came
  out of it are in the entry below.

  Two premises in this entry were wrong and are corrected rather than quietly dropped. The
  analytic estimate it refers to DOES exist, in `docs/enforcement-plane.md` §2. And
  `adce_epoch_read` has NO retry loop — zero `while` statements; on a torn read it returns 0
  and the caller does not read again. The only loop in the read path is the entry spin.

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
- A FUTURE `observed_at_ns` reads as maximally stale, and **the explicit
  `observed_at_ns > now_ns` branch in `adce_enf_classify_stale` is what produces it** on
  every path the gate takes. A reader that cannot order the publication it read against the
  clock it read has no coherent view of time and must not act on that publication, so that
  branch is load-bearing rather than an oversight: DELETE IT and `enf_stale_route_classify`
  and `enf_stale_route_identity` both fail. That is not an argument, it was run — mutation
  M05b of the README campaign. A torn `adce_epoch_read` counts as stale for the same reason —
  no snapshot means no advice — and deleting the `have_snapshot` branch fails the same case.
  Both are reachable without any fault and both require a concurrent publication, so nonzero
  `stale_reads` on a healthy system is expected. Measurements in
  `docs/enforcement-plane.md` §4.1. This is a fail-closed contract; changing it is an API
  decision.

  **This entry used to credit the unsigned wrap in `adce_epoch_is_stale`, and that was
  false.** Replacing the wrap with a branch returning 0 — the exact mutation the old text
  warned about — leaves `enf_stale_route_classify` GREEN, because the gate does not call that
  function. The wrap was load-bearing until `adce_enf_classify_stale` landed and took over the
  ordering decision; the entry was never updated. The class of error is recorded below.
- **A SENTENCE CAN NAME A REAL FUNCTION, DESCRIBE ITS BEHAVIOUR EXACTLY, AND STILL BE FALSE
  ABOUT THE SYSTEM.** The class behind the `adce_epoch_is_stale` correction above, and the
  reason it is recorded as a class rather than as one fixed paragraph.

  **The measurement first.** Counted with comments stripped, so a mention and a call are not
  confused:

  | | shipping (`include/` + `src/`) | tests |
  |---|---|---|
  | `adce_epoch_publish` | 1 call | 19 calls |
  | `adce_epoch_read` | 1 call | 17 calls |
  | **`adce_epoch_is_stale`** | **0 calls** | **14 calls** |

  In shipping code the name appears six times: the definition in `adce_platform.h`, and five
  MENTIONS, every one of them inside a comment — two in `adce_enforce.h`, one in
  `adce_observe.h`, one in `src/adce_observe.c`, one in `src/adce_obs_thread.c`. `src/` calls
  it zero times. Every one of its fourteen call sites is in `test/`. The locked decision above
  names three functions as implementing one pattern; two of them still do.

  **What makes this a distinct failure and not an ordinary stale comment.** Every word of the
  old sentence about `adce_epoch_is_stale` was true OF THAT FUNCTION. It does compute an
  unsigned difference; the difference does wrap on a future timestamp; the wrap does make it
  return "stale". Nothing about the function changed, nothing about it was misdescribed, and a
  reader checking the sentence against the function would confirm it line by line and come
  away satisfied. **What changed is which path the code takes.** `adce_enf_classify_stale`
  landed and made the ordering decision itself, with an explicit branch, and from that moment
  the gate stopped going through the function the prose was pointing at. The sentence
  survived because it was never about the wrong function — it was about the wrong ROUTE.

  **Neither a reader nor a grep can reach this.** A reader has the sentence and the function,
  and the two agree. A grep has the name, and the name is present in shipping code — six
  times — so no pattern over the text distinguishes a live mechanism from a commented-out
  memorial to one. This is the same wall the printed-universal audit hit and for the same
  reason: what separates a true sentence from a false one here is a fact about which code
  executes, and no property of the text encodes it. Call-graph tooling would find the zero
  call sites but not that a paragraph depends on them.

  **It was reachable by exactly one method: breaking the thing the sentence claims is
  enforced.** Mutation M05a of the README campaign replaced the wrap with the very branch the
  old text warned would fail open — `if (observed_at_ns > now_ns) return 0;` — and
  `enf_stale_route_classify`, the case the paragraph named, stayed GREEN. One case fired,
  `enf_stale_route_equivalence`, and it was not the one cited. Fourteen other mutations in the
  same campaign fired the case the page named; this one did not, which is what isolated it.

  **That is what the README's binding rule bought, and it is the reason to keep paying for
  it.** The rule — every load-bearing claim names the assertion that enforces it or is marked
  unenforced — looked like a documentation convention. It is not. Naming an assertion makes a
  prose claim EXECUTABLE: the sentence acquires a falsifier, and the falsifier can be run.
  Without the name there is nothing to break and the error is undetectable by any means short
  of reading the whole call graph and the whole page together. This is branch 3 of the
  printed-universal rule applied to prose rather than to numbers: remove the ambiguity so the
  claim becomes checkable.

  Standing consequence: **a prose claim that names a mechanism should name the assertion that
  fails when the mechanism is removed, not the function whose behaviour it describes.** Those
  are different things, and this entry exists because they were confused once.

- **OPEN API QUESTION, proposed and NOT decided: what is `adce_epoch_is_stale` for?**
  Recorded here rather than acted on, because it is `static inline` in a public header and
  removing or changing it is an API decision, which the working agreement requires to be
  proposed and waited on. Nothing about it is changed in the commit that adds this entry.

  The question is exactly two-sided and the evidence does not settle it:

  1. **It is public API with no internal user** — a predicate offered to consumers who read
     the epoch themselves rather than through `adce_enf_decide`, in which case zero internal
     call sites is the design working and the fourteen test call sites are its coverage. If
     so, it needs a comment saying so, because nothing currently distinguishes this case from
     the other one.
  2. **It is dead code with a test suite attached** — superseded by `adce_enf_classify_stale`,
     which computes strictly more (three routes rather than one bit) and is what the gate
     runs. If so, the fourteen tests are testing something no shipping path executes, and
     `enf_stale_route_equivalence` is pinning the gate to a predicate the gate abandoned.

  What tips it slightly toward (1): `docs/enforcement-plane.md` §4.1 and
  `docs/observation-plane.md` both describe consumer-side staleness checking, and a
  header-only library that publishes an epoch state should plausibly publish the predicate for
  reading it. What tips it toward (2): `adce_enf_classify_stale` returns `ADCE_ENF_FRESH` for
  exactly the cases `adce_epoch_is_stale` returns 0, so any consumer wanting the bit can take
  it from the classifier and get the route for free.

  **Do not resolve this by deleting the function while tidying something else.** If it is
  removed, `enf_stale_route_equivalence` loses its reference predicate and the equivalence
  argument that justified the classifier's landing has to be restated against something else.
- **The internal-use check, and the measurement taken BEFORE it was asserted.**
  `scripts/check-internal-use.sh`: every function in `include/` must have a call site in
  `include/` or `src/`, comments and string literals stripped and the definition excluded,
  or carry `ADCE_PUBLIC_NO_INTERNAL_USER: <reason>` above its declaration.

  **THE MECHANISM IT CLOSES IS THE TEST SUITE, and that is broader than the
  `adce_epoch_is_stale` instance.** A dead function with a test suite attached reads as
  maximally live from every angle except the one that matters. `adce_epoch_is_stale` had
  fourteen test call sites and zero shipping ones: it compiled, it was covered, its tests
  were green, and two documentation paragraphs pointed at it as a live mechanism. Coverage
  is what kept it warm. **The check therefore counts test calls at ZERO weight, on purpose.**

  This is not one function's problem. Measured on the same tree:

  | | shipping call sites | test call sites |
  |---|---|---|
  | `adce_epoch_publish` | **1** | 19 |
  | `adce_epoch_read` | **1** | 17 |
  | `adce_epoch_is_stale` | 0 | 14 |

  Two of the three are **one refactor from the third**, with nothing in the gate that would
  turn red on the way. That is the standing exposure, not a hypothetical.

  **THE MEASUREMENT, and it is not the hoped-for result.** The predicate as first specified
  — call sites in `src/` only — fires on **27 of 37** functions. That is unusable, and worse
  than the printed-universal grep's 66. The cause is that this is a header-only library: the
  Enforcement Plane has NO `.c` file by locked decision, so every `adce_enf_*` function is at
  zero by design, and the platform layer's internal callers are other headers rather than
  `src/`. Counting `src/` alone measures the wrong graph.

  Corrected to count `include/` **and** `src/`, it fires on **14 of 37**, which sort into
  three groups:

  | group | n | verdict |
  |---|---|---|
  | consumer entry points (`adce_obs_tap`, `adce_enf_admit`, `adce_enf_thread_init`, `adce_obs_thread_start`) | 4 | correct; zero internal callers IS what an entry point is |
  | the published Q16 lane (`adce_q16_*`) | 8 | correct; consumers receive `pressure` in Q16 and the library converts with a private cast |
  | genuinely open (`adce_epoch_is_stale`, `adce_rng_next_unit`) | 2 | the finding |

  Twelve are annotated. **Two are left red deliberately**, because annotating them would
  answer an API question the working agreement requires to be proposed and waited on. The red
  IS the open question, held visible until it is answered.

  So: it does NOT fire on `adce_epoch_is_stale` alone. It fires on fourteen, thirteen of
  which are explained on their first encounter and stay quiet afterwards. Recorded this way
  round because measuring after asserting is how a number gets fitted to a conclusion, which
  this document already has three entries about.

- **WHY THIS PREDICATE IS MECHANISABLE WHERE THE PRINTED-UNIVERSAL ONE WAS NOT. Do not read
  that negative result as "gates do not work here."** The two differ in kind, not in degree,
  and the difference is where the predicate lives.

  | | printed-universal rule | internal-use check |
  |---|---|---|
  | the question | is the quantity behind this sentence asserted anywhere? | does a call edge exist from shipping code to this function? |
  | evaluated over | English prose | the code |
  | the answer lives in | the TEST SUITE — a different artefact from the one being judged | the same artefact being judged |
  | decidable from the input? | **no** — no property of a sentence determines it | **yes** — exactly, by parsing |
  | phrasing-dependent | yes | no |
  | firing profile | 66 per document, forever, true-positive rate zero | once per function, ever |

  The printed-universal predicate failed because it had to cross from text to test suite and
  nothing in the text encoded the crossing. That is a statement about THAT predicate. This one
  never leaves the code: a call edge is a syntactic fact with an exact answer, and no wording
  anywhere can change it.

  **The cost profiles differ in the same direction and this is what makes it landable.** The
  boundary note fires on 71% of pull requests forever, which is why its evaluation counts
  escape uses after ten pull requests. This check fires once per function and is then quiet
  until the answer changes — a one-time burst of thirteen, not a recurring prompt. A
  recurring prompt gets answered by reflex, which is the failure this project condemns; a
  one-time burst does not. **No ten-PR evaluation is scheduled for this check**, deliberately,
  because there is nothing recurring to measure. If it does decay it will show as annotations
  accumulating with empty reasons, which is what requiring the reason after the marker is for.

  Neither check is in the ruleset's required contexts, so both show red without blocking.

- **SECOND OPEN API QUESTION, found by the check on its first run: what is
  `adce_rng_next_unit` for?** Proposed and NOT decided; nothing about it is changed.

  It has zero shipping call sites and one test call site. Its own comment in
  `adce_platform.h` says it is "confined to the Observation Plane by convention", and
  `src/adce_observe.c` does not call it — the same shape as `adce_epoch_is_stale`, where prose
  names a real function that behaves exactly as described and the code takes another route.
  Either it is public API for consumers doing their own floating-point work in the
  Observation Plane, in which case the convention comment should say that instead of
  describing an internal home it does not have; or it is dead, in which case the lane
  convention it anchors — `adce_enforce.h` and this document both cite it by name as the thing
  the Enforcement Plane must not use — is anchored to nothing.

  Note the asymmetry before deciding: the convention it names is still doing work even if the
  function is not. "No `double` and no `adce_rng_next_unit`" is a live rule about the
  Enforcement Plane whether or not anything calls the function.

- **THE INTERNAL-USE CHECK LANDED, AND THE REBASE ONTO #38 WAS ITS FIRST MUTATION TEST.**
  #26 sat unmerged for four days while the API surface it measures moved underneath it. The
  rebase is therefore not a chore — it is the only opportunity this gate will ever have to be
  tested against annotations written by someone who could not run it.

  **#38 WROTE TWO ANNOTATIONS BLIND, AND BOTH PASSED ON FIRST CONTACT.**
  `adce_obs_claim_counter` and `adce_obs_residual` were marked
  `ADCE_PUBLIC_NO_INTERNAL_USER` while no gate existed to read the marker — filed item 6, a
  marker with no gate. On the rebased tree the predicate parsed both, extracted both reasons,
  and listed them as declared rather than as defects. **Nothing was adjusted to make that
  happen; the format survived transmission without enforcement.**

  **n = 2, and that caveat is the whole of what may be concluded.** Two annotations by one
  author in one pull request, written from the spec in a document rather than from the
  predicate. It is evidence that the format is writable from its description — which is a
  real property and the one filed item 6 doubted — and it is not evidence that an unenforced
  convention survives generally. The next unannotated symbol is a fresh sample, and the check
  is now present to take it.

  **RE-MEASURED ON THE REBASED TREE RATHER THAN CARRIED.** The 14-of-37 figure was taken
  against the pre-#38 surface; both numbers below were produced by running the script, the
  old one against a reconstructed pre-#38 tree carrying #26's original annotations:

  | | functions in `include/` | shipping caller | annotated | UNEXPLAINED |
  |---|---|---|---|---|
  | before #38 (`9ac1713`) | 37 | 23 | 12 | 2 |
  | after #38 (this branch) | **40** | **24** | **14** | **2** |

  It fires on 16 of 40 where it fired on 14 of 37. **The delta is read off the diff, not
  argued:** #38 added exactly three functions to `include/` and removed none —
  `adce_obs_claim_counter`, `adce_obs_drain`, `adce_obs_residual`. `adce_obs_drain` has
  shipping callers in `src/adce_obs_thread.c` and `src/adce_observe.c`, so it joins the live
  column; the other two have none and are annotated. 23+1, 12+2, and the two reds untouched.
  **No previously-live function fell to zero, and nothing new went red.**

  **THE THREE GROUPS DO NOT STILL HOLD, AND THAT IS THE FINDING.** They were: four consumer
  entry points, eight Q16 lane functions, two genuinely open. `adce_obs_claim_counter` lands
  cleanly in the first — claiming a counter is the consumer's act exactly as tapping is, so
  the group is now five. **`adce_obs_residual` lands in none of them**, and its own annotation
  says so in its own words: *"the residual term of the overrun identity, which only a test
  evaluates"*.

  A public function whose only caller is a test is the exact shape of
  `adce_epoch_is_stale` — category (2) of the open API question above, "dead code with a test
  suite attached" — and it is sitting in the GREEN column behind a well-formed annotation.
  That is not a defect in the predicate and the annotation is not dishonest; the reason is
  true, and the gate cannot distinguish "a consumer calls it" from "a test calls it" because
  that distinction is not in the call graph it reads.

  **What actually happened is the gate working on its first day.** It forced a reason to be
  written, and the written reason discloses the problem. Left alone, `adce_obs_residual` would
  have looked internally-unused-by-design forever. **Recorded as a THIRD OPEN API QUESTION and
  deliberately NOT resolved here:** changing or removing a public function is an API decision
  that must be proposed and waited on, and this pull request's scope is landing the gate. The
  question is whether `adce_obs_residual` is a consumer-facing auditing accessor — in which
  case its annotation should say that instead of naming a test — or test scaffolding that
  should not be in a public header.

  **THE INTERPRETER IS A HARD DEPENDENCY, AND ITS ABSENCE IS NOW A LOUD, ATTRIBUTING
  FAILURE.** Settled from the scripts rather than from intent.

  *Where it is required:* **CI only.** `.github/workflows/verify.yml` is the sole invoker.
  Neither `scripts/verify.sh` nor `scripts/verify-linux-gcc.sh` nor anything under
  `scripts/hooks/` runs this script, so macOS not guaranteeing `python3` does not affect the
  development gate. A developer running the check by hand needs it; no gate on that host does.

  *What happened when it was absent, measured by removing `python3` from `PATH`:* the script
  already failed — `set -e` carries bash's exit 127 out, so it never passed and never skipped,
  which is the important half. But everything it printed was

      ./scripts/check-internal-use.sh: line 48: python3: command not found

  **a line of shell, not a gate.** This document already ranks an unattributable red as a
  defect of its own — it is why the stale-posture failure prints `aged=N` beside torn and
  future rather than a bare assertion number. A red that does not say what it was checking
  invites the reading that the check is broken, and the step after that reading is to skip it.

  Repaired with an explicit preflight that names the gate, says the check DID NOT RUN and has
  no answer to report, states where it is expected to run, and says there is no flag that
  turns it into a pass. Both the absent-interpreter path and the two-open-questions path exit
  1, with entirely different messages, so the two reds can never be confused. **There is
  deliberately no environment variable that converts this to a skip**: a gate that passes
  because an interpreter is missing is the class this project ranks worse than no gate at all.

  **THE FIVE-PULL-REQUEST ANSWER WINDOW STARTS AT THIS MERGE, and it has never run before.**
  The two reds — `adce_epoch_is_stale` and `adce_rng_next_unit` — stay unannotated by
  decision. The red IS the open API question, held visible rather than papered over with a
  marker.

  **The cost of a proposed-but-unlanded gate, measured.** #26 was opened 2026-09-10 and
  **twelve pull requests merged before it** — #23, #24, #25, #29, #31, #32, #33, #34, #35,
  #36, #37, #38 — every one of them under a check that existed only as a branch. For that
  whole window this document's five-gate table listed `internal-use` as a gate, and #38
  annotated two new public functions for it and reasoned about the annotation as a
  cross-gate interaction. **A proposal reads as a control to everyone downstream of it.**
  That is a sharper version of the same failure this document already collects: a gate that
  is silent is indistinguishable from a gate that passed, and a gate that is *unmerged* is
  indistinguishable from a gate that is *present* to anyone reading the record rather than
  the workflow. #38 caught it only by grepping the workflow and finding the string exactly
  once, inside a comment. The window starts now because now is the first moment there is
  anything to measure.
- **A NEW GATE'S FIRST DUTY IS TO BE SHOWN FAILING on a change it must catch, evaluated in
  this repository's actual file layout, before it is proposed.** The standing rule the
  `boundary-note` defect produced, and the defect itself.

  `boundary-note` shipped in #24 selecting changed files with `^(src|test)/.*\.c$`. `include/`
  was excluded, so the job was INERT for the entire Enforcement Plane — which is
  `include/adce_enforce.h` with no `.c` file at all, by locked decision — and for the
  header-only platform layer. PR #26 changed four headers and the job printed
  `No src/*.c or test/*.c changed. Nothing to ask about.`

  **The gate was weakest exactly where the evidence is densest.** The plane it could not see
  is the one holding the most locked decisions in this document: the inline-only construction
  and its RNG-stream reason, the stale-route classifier and its three counters, the
  fail-closed future branch, the deployment tuning block, `ADCE_ENF_STALE_PRESSURE`. A
  boundary moving there is the most likely kind to need recording and was the one kind the
  job could not detect. That is not bad luck. The selector was written from where its author
  assumed the code lived, and this document's own layout note says in its own words that a
  reader looking for `src/adce_enforce.c` should stop looking.

  **What was done instead of a mutation proof, and why it was not enough.** The job was
  branch-tested when it landed: each of its four branches was driven with real SHAs and each
  behaved correctly. Every branch was right. **Branch coverage of a script is evidence about
  the script, not about the repository.** The defect was not in any branch; it was in the
  premise that `src` and `test` enumerate the code, and no amount of exercising the branches
  can reach a premise none of them tests.

  So the rule, and it applies to every gate proposed here from now on: before a gate is
  proposed, construct a change it MUST catch, in the real tree, and show it going red. A
  green run proves nothing. A red run on a fabricated input proves nothing either if the
  input was not shaped by the layout the gate will actually meet.

  Repaired to `^(src|include|test)/.*\.(c|h)$`, with M1/M2/M3 in the gate commit's message:
  fires on a header-only change, stays quiet on a docs-only change, and the old selector
  produces an EMPTY MATCH on M1's file list — the last being what identifies this as the
  repair for this hole rather than for some other one.

- **How many gates in this repository check less than they appear to, and why the ordinal is
  not asserted here.** The repair above was requested as "the fifth inert gate". That count
  could not be reproduced from this document, so the enumeration is recorded instead of the
  ordinal — which is this project's own rule about figures applied to its own record.

  | # | gate | what it does not do | on `main`? |
  |---|---|---|---|
  | 1 | `CodeRabbit` | renders a pass having reviewed nothing | yes |
  | 2 | `strict_required_status_checks_policy: false` | lets a PR merge on checks that ran against a branch behind `main` | yes |
  | 3 | GCC's TSan | runs nowhere; every race result here is Clang's | yes |
  | 4 | `boundary-note` | was blind to `include/`; also non-required, so its red blocks nothing | yes |
  | 5 | `internal-use` | non-required, so its red blocks nothing | no — open in #26 |

  **Four on `main` by the "exists and checks less than it appears to" rule, five if the
  unmerged one counts, and the ordinal depends entirely on which rule is used.** Both
  readings are defensible and neither is derivable from the document as it stood. The number
  is left as an enumeration so the next reader can recount rather than inherit.

  The pattern across all five is worth more than the count: **not one of them is wrong. Every
  one is silent.** A gate that fails loudly gets fixed. These pass, skip, or never run, and
  the green tick is indistinguishable from the green tick of a gate that did its job. This
  document already records that a silently-skipping profile is worse than one that does not
  exist; five instances say that is the default failure mode here rather than an exception.

- **SECOND HOLE, NAMED AND NOT FIXED: the escape counter counts its own definition.** Found
  while verifying that the `boundary-note` repair leaves the pre-registered evaluation intact.

  The evaluation command is `git log --grep='no boundary change' --oneline main`. Run on
  `main` today it returns **1**, and that one is `e37c33c` — the commit that PROPOSED the
  gate, whose message explains the escape phrase twice. No escape has ever been used. The
  instrument counts any commit that DISCUSSES the escape, including the commit that defines
  it, and including this entry's own commit once it lands.

  The count is therefore already wrong before the window has meaningfully opened, and it is
  wrong in the direction that would retire the gate early: the pre-registered rule says a high
  count means the check is a ritual and must be REMOVED. A self-polluting counter would build
  the case for removal out of commits that are talking about the check rather than escaping it.

  **Not fixed here**, deliberately: this pull request is scoped to the selector, and a change
  to the evaluation instrument is a change to a pre-registered experiment, which deserves its
  own commit and its own argument rather than being folded into a repair of something else.

  **It does not affect whether the repair preserves the window.** The escape grep, phrase and
  guidance wording are byte-identical across the repair, so the counter reads the same before
  and after — equally wrong both sides, which is what comparability requires. What the repair
  does change is EXPOSURE: more pull requests are now subject to the check. With zero genuine
  escapes recorded and one of ten pull requests elapsed, there is no accumulated tally to
  reset, so the window continues rather than restarting. If the widened population is judged
  to be a different experiment, the window should be restarted deliberately and that decision
  recorded here.

- **THE ESCAPE PREDICATE COUNTED PROSE, AND IT DID SO ON BOTH SIDES OF THE GATE.** The
  `boundary-note` evaluation instrument, repaired, and the pre-registered window restarted.

  **Only half the defect had been named.** The counting command was
  `git log --grep='no boundary change' --oneline main`, which matches any commit that
  DISCUSSES the escape. The job's own escape branch carried the identical looseness —
  `grep -qi 'no boundary change'` over every message in the range — so **a commit that merely
  talked about the escape SATISFIED THE GATE.** Repairing the counter alone would have left
  the two disagreeing, with a message able to pass the check and not be counted. Both now run
  one predicate.

  **The instrument was degrading in plain sight, and the direction is the bad one.** It
  returned 1 when the hole was first named and 2 a day later: `e37c33c`, which proposed the
  gate and explains the phrase twice, and `898c293`, which repaired the selector and discussed
  the phrase again. Every commit written in defence of the check incremented it, and a HIGH
  count triggers REMOVAL — so the instrument was assembling the case to retire the gate out of
  the commits arguing for it. Under the strict predicate `main` reads **0**, which is the
  truth: no escape has ever been used.

  **The strict form**, in column 1, with a colon and a non-empty reason. Quoted or indented
  prose escapes nothing, and an escape that is not seen leaves the gate RED, which is the safe
  direction.

  | | predicate |
  |---|---|
  | job | `grep -qiE '^no boundary change: *[^[:space:]]'` |
  | counter | `git log -i -E --grep='^no boundary change: *[^[:space:]]' --oneline main` |

  Mutation-proved both directions against real commits. A probe carrying a genuine escape is
  counted by the new command and passes the job; a probe that only discusses the mechanism is
  NOT counted, and — the part that matters — passes the OLD job while the NEW job holds the
  gate red at exit 1.

  **THE WINDOW RESTARTS AT ZERO: ten CODE-TOUCHING pull requests merged after the repair
  lands.** Three reasons, and the third alone is sufficient.

  1. **The population changed.** The selector went from `^(src|test)/.*\.c$` to
     `^(src|include|test)/.*\.(c|h)$`, retrospectively 16 of 24 merges firing against 19 of 28.
     A different denominator is a different experiment.
  2. **The instrument changed.** A pre-registration whose instrument is adjusted mid-run and
     whose tally is kept is not a pre-registration; it is a result chosen after the fact with a
     procedural label on it.
  3. **The window was NEVER VALID.** Its counting command was broken from the day it was
     written, so there was never a tally to carry — only a number that grew when people wrote
     about the check. Reasons 1 and 2 describe a window being invalidated. This one says there
     was nothing to invalidate.

  "Code-touching" now has a definition rather than a reading: the selector matched, i.e. the
  job did not take its quiet branch. The workflow comment carries the command that replays the
  selector over merges, so the denominator is countable from the repository instead of
  remembered.

- **"WRITTEN IS NOT RUN", THIRD INSTANCE, AND THE WORST-SHAPED ONE: a pull request that CI
  cannot structurally reach.** The first two instances were an unexecuted committed code path
  (the Darwin `getentropy` loop) and an unexecuted command snippet (the quick syntax check).
  This one is different in kind.

  `pull_request: branches: [main]` meant a pull request targeting any other branch received
  **no CI at all** — not a reduced set, not a skip, nothing. The three required contexts were
  never created, so the ruleset had nothing to wait on.

  **What separates it from the other two: choosing does not fix it.** An unexecuted branch in
  `adce_platform.h` and a broken snippet in Commands both sit in the tree, and anyone who
  decides to run them can. Here the work is written, committed, pushed and visible on a page,
  and no amount of choosing runs it. The gap is not in what was executed but in what was
  reachable.

  **And it reads as PENDING rather than as IMPOSSIBLE.** A pull request with no checks is
  indistinguishable on the page from one whose checks have not started. PR #27 held a written
  repair for the `boundary-note` selector and sat in that state for two days:

      $ gh pr checks 27
      CodeRabbit  pass  0  Review skipped: reviews are disabled for this base branch

  One advisory tick that does not come from this workflow, and not one of the three required
  contexts. The page does not say "these cannot run here". It says nothing, and nothing reads
  as "not yet". That is the same defect class this document already ranks worst — a
  silently-skipping profile — arriving through the CI configuration rather than through a
  script.

  Repaired by dropping the branch filter, and mutation-proved with a throwaway pull request
  against a non-main base: zero of the three required contexts before, all three plus
  `boundary-note` after. The cost is that stacked and experimental pull requests now consume
  the full matrix, about two and a half minutes each. **A check that cannot run is worth less
  than a check that costs something.**

  Note what this does NOT fix, because the stack in #26-#28 is the standing example: a pull
  request whose base is another pull request's branch now gets CI, but it is CI against a base
  that has not merged. Green there is evidence about the stack, not about `main`.

- **PR #2 ASSESSED AND CLOSED: its premise was superseded, and the correction it proposed
  points the opposite way from the one that landed.** Recorded because the branch carried a
  measurement worth keeping and a figure that must not be inherited.

  PR #2 (2026-09-04, `claude/harness-recovered-stale-bound`) proposed multiplying
  `harness_publications_bound` by `HARNESS_STALE_BATCH`, from `dur/T + 1` to
  `(dur/T + 1) * 256`. Its premise: `harness_stale_ingress_main` runs 256 reads with no
  pacing inside a batch, so nothing bounds how many of them land inside ONE publication's
  seqlock write window, and a thread's torn+future count can therefore reach a whole batch.
  Its evidence: one observed failure at `HARNESS_PH_RECOVERED` with `stale_reads = 261`,
  which is almost exactly the batch size.

  **It rebases cleanly onto `main`. That is not the question.** The derivation does not
  survive, on three independent grounds.

  **1. The premise was answered by the route split, in the other direction.** PR #2 could not
  classify the 261 — its own body says "Classification attempted, not obtained" — so it
  attributed the whole count to torn/future. The classifier landed later, and
  `harness_check_live_phase` now attributes exactly this shape to AGED: a route that needs no
  publication at all, is bounded by ARRIVALS, and contributes 256 per batch against a bound of
  ~31. `docs/enforcement-plane.md` §4.1 says the same. **PR #2 would have loosened the
  torn/future bound 256-fold to accommodate a count that was not torn/future.** The landed fix
  keeps the publication bound for the pair that genuinely needs a concurrent publication and
  asserts `aged == 0` separately, which is tighter on both halves rather than looser on one.

  **2. The bound it wanted to relax is not under strain. Measured on current `main`,
  ADCE_REPEAT=3 under all three profiles:**

  | test | unpaced batch | bound | torn+future observed |
  |---|---|---|---|
  | `harness_stale_posture` | 256 | 31 | **1** peak, per thread per phase |
  | `harness_concurrent` | 1024 | 32 | **2** aggregate over four threads |

  **3. It conflicts with an assertion `main` already runs, and the conflict is the
  falsification.** `harness_concurrent` drives a **1024**-read unpaced batch — four times PR
  #2's — against `torn + future <= publications + 1`, which is TIGHTER than the bound PR #2
  called unhonourable. If a whole unpaced batch could straddle one write window, that
  assertion would break first and by more. It does not break, on either architecture, under
  TSan, across every run in this project's history.

  So: **premise superseded, bound not needed, and contradicted by a stricter assertion that
  already passes.** Closed rather than rebased.

- **THE OBSERVER-EFFECT FINDING FROM PR #2 IS NOT A MEASURED NEGATIVE RESULT, and recording
  it as one would enshrine this project's own recurring error.** Kept here because the
  discrepancy is the part that would otherwise die with the branch.

  PR #2's body reports "zero failures in 1000 combined executions against a measured ~31%
  (154/500) baseline on the uninstrumented binary", and argues from that gap that the failure
  is an instruction-level race rather than a coarse OS-scheduling stall — on the reasoning
  that a starved closer would not be rescued by a few nanoseconds of unrelated code.

  **`154/500` has two incompatible readings in this project's record, and PR #2 contains
  both.**

  | reading | rate | where |
  |---|---|---|
  | 154 failures in 500 runs | 30.8% | PR #2's own "~31%" |
  | one failure at iteration 154 of a planned 500 | 0.65% | the entry above, and PR #2's own "the assertion failed **once**" |

  The body says "the assertion failed once" and "~31% (154/500)" about the same experiment.
  Those cannot both be true.

  **The 31% reading is refuted by a measurement already in this document.** The entry above
  re-ran the uninstrumented TSan binary 400 times, runner trimmed to `harness_stale_posture`,
  and got zero failures. At a true rate of 30.8%, zero in 400 has probability
  `0.692^400 ~ 1e-64`. The high reading cannot stand.

  **Under either reading, no observer effect is established.** Under the 31% reading the
  baseline is refuted. Under the 0.65% reading both arms are empty — 0/400 instrumented
  against 0/400 uninstrumented — which is what the entry above already concludes: the
  suppression hypothesis is unsupported rather than demonstrated.

  **What follows is worth more than the arithmetic, and it is the reason to record this at
  all.** PR #2 used that gap to DISCARD the coarse-scheduling explanation in favour of an
  instruction-level race. The route split later attributed the event to **aged** — which is
  the coarse-scheduling explanation exactly: an epoch that stopped advancing because the
  closer did not publish. **The hypothesis PR #2 rejected is the one that turned out to fit,
  and it was rejected on the strength of a figure that does not hold up.** A number with no
  basis did not merely sit in a document here; it steered a diagnosis away from the right
  answer.

  Do not quote 31%, 154/500, or 1000 executions as rates. They join 0.067, 0.65% and 261 as
  figures whose supporting event cannot be reproduced — the fifth instance of one observation
  presented as a rate, and the first where the bad number changed a technical conclusion
  rather than only a sentence.

- **THE CONTENDED TAP, MEASURED. §4's 27-59 M arrivals/s per thread is OVERTURNED for
  multi-threaded ingress, and the tap imposes a GLOBAL ceiling on observable arrivals that
  adding threads does not raise.** Entry (1) of the unverified list, retired.

  **Pre-registered before measuring**, in `bench/tap_contention.c`, committed in its own
  commit with no numbers in it; `git log` on that file is the proof of order. Threads in
  {1,2,4,8}; arrivals held fixed PER THREAD; a spin barrier so all threads are in the loop
  together; median over thread-samples with min and max, never the mean, because a
  descheduled thread is a one-sided outlier describing the host rather than the tap.

  **Two arms, and the control is what makes the result attributable.** SHARED is the shipped
  configuration — one counter, T writers. PRIVATE gives each thread its own counter: same
  instruction, same atomic, same alignment, no sharing. If the atomic RMW itself were the
  story both arms would rise together.

  | host | n/cell | arm | T=1 | T=2 | T=4 | T=8 | T8/T1 |
  |---|---|---|---|---|---|---|---|
  | M3 Darwin arm64, 8 core | 10 runs | SHARED | 1.74 | 5.98 | 17.79 | 134.22 | **77x** |
  | | | private | 1.74 | 1.77 | 1.93 | 2.13 | 1.2x |
  | `ubuntu-24.04` x86_64, 4 vCPU | 5 runs | SHARED | 2.19 | 22.95 | 67.16 | 124.37 | **57x** |
  | | | private | 2.20 | 2.22 | 2.39 | 3.25 | 1.5x |
  | `ubuntu-24.04-arm` aarch64, 4 vCPU | 5 runs | SHARED | 4.22 | 14.02 | 26.92 | 48.84 | **12x** |
  | | | private | 4.22 | 4.21 | 4.22 | 5.47 | 1.3x |

  Median ns per arrival. Cell n is runs x T thread-samples, so 5 at T=1 and 40 at T=8 on CI.

  **The prediction was pre-registered as hypothesis B — SHARED rises at least 3x from T=1 to
  T=8 while PRIVATE stays flat — and it HELD on all three hosts, by 12x to 77x rather than
  3x.** The padding works: `_Alignas(ADCE_CACHELINE)` prevents FALSE sharing, and the private
  arm proves it by staying flat. What padding cannot prevent is TRUE sharing, and
  `memory_order_relaxed` does not help — it removes ORDERING, not COHERENCE, so every
  increment still migrates the line.

  **THE VERDICT ON §4.** The published band is 17-37 ns per arrival from gate + clock,
  giving 27-59 M arrivals/s per thread. Adding the measured tap to each host's own gate and
  clock figures from `enforcement-plane.md` §5:

  | host | gate+clock | T=1 | T=2 | T=4 | T=8 |
  |---|---|---|---|---|---|
  | M3 | 18.0 ns | 50.7 | 41.7 | 27.9 | **6.6** |
  | x86_64 CI | 21.8 ns | 41.7 | **22.3** | **11.2** | **6.8** |
  | arm64 CI | 36.6 ns | **24.5** | **19.7** | **15.7** | **11.7** |

  M arrivals/s per thread; bold is below the published floor of 27. **On the shipping target
  at four ingress threads — the configuration `t_adce_harness.c` actually runs — it is 11.2
  against a published 27-59, low by 2.4x to 5.3x.** On arm64 CI the band is missed even at
  T=1, because that host's clock alone is 30.6 ns.

  **The finding that is larger than the per-thread one: aggregate throughput does not
  scale.** Median aggregate across the SHARED arm, M arrivals/s over all threads:

  | host | T=1 | T=2 | T=4 | T=8 |
  |---|---|---|---|---|
  | M3 | 575 | 323 | 203 | 59 |
  | x86_64 CI | 456 | 86 | 59 | 58 |
  | arm64 CI | 237 | 137 | 144 | 147 |

  The private arm scales nearly linearly over the same sweep (M3: 576 to 3554). So the one
  counter is a GLOBAL CEILING on how fast arrivals can be observed at all, and on two of
  three hosts aggregate throughput DEGRADES as threads are added rather than merely
  saturating. Adding ingress threads past two buys nothing and costs throughput.

  **NO ASSERTION IS WRITTEN, and that is branch 2 rather than an omission.** A threshold on
  any of these numbers would be a band with no derivation evaluated on a shared runner, which
  is the failure mode this list exists to prevent — the same reason `t_adce_latency.c`
  asserts nothing. Every figure above carries its n. Branch 3 is unavailable: the quantity is
  a property of the host's cache coherence, and there is no construction that removes the
  nondeterminism the way the clamp regime did.

  **What this does not settle.** The loop does nothing but tap, so it is the worst case for
  line migration; a real ingress site interleaves gate, clock and request work between taps.
  And the composition above assumes the three terms ADD. Neither is tested here. What is no
  longer in doubt is the mechanism, the direction, and the order of magnitude — the omitted
  term is comparable to or larger than the entire sum §4 published, at every T >= 2 on every
  host measured.

- **DESIGN CONSEQUENCE, NOW IMPLEMENTED: the Ingest Plane's single shared counter was a
  scaling defect, and the Enforcement Plane had already taken the opposite decision on the
  same question.** The evidence is the entry above. The proposal below is kept in the shape
  it was proposed in, with the outcome recorded beneath it, so that what was predicted can
  be read against what happened.

  **The measurement, restated as the design claim it supports.** Aggregate tap throughput on
  `ubuntu-24.04` x86_64, median over 5 runs per cell, M arrivals/s over all threads:

  | arm | T=1 | T=2 | T=4 | T=8 |
  |---|---|---|---|---|
  | SHARED — one counter, the shipped configuration | 456 | 86 | 59 | 58 |
  | private — one counter per thread, the control | 454 | 709 | 1638 | 1485 |

  The shared arm loses 87% of its throughput between one thread and two and never recovers.
  The private arm rises to 1638 at T=4, which is the runner's core count, and dips to 1485 at
  T=8 where 4 vCPU are oversubscribed — so it is near-linear up to the hardware and then
  flat, which is what a design without a shared line looks like. On the 8-core M3 the private
  arm is near-linear across the whole sweep, 576 to 3554.

  **What is duty-cycle dependent and what is not, stated separately because conflating them
  would overclaim.** The benchmark taps back to back, which is the WORST case for line
  migration; a real ingress site puts the gate, the clock and request work between taps, and
  the per-tap penalty there will be smaller. **The MAGNITUDE is therefore not transferable.
  The SIGN is.** At any duty cycle, a shared line makes every increment pay for ownership
  transfer that private counters do not pay, and the rate at which one line can change hands
  is a ceiling that private counters do not have. Sharing is never the better arrangement at
  any duty cycle; only the size of the gap moves. The comparison that holds is SHARED against
  PRIVATE at a fixed duty cycle, not T=1 against T=8 at fixed sharing — at a low enough duty
  cycle the shared arm does still scale, because the line is not saturated, and saying
  otherwise would be the stronger claim the measurement does not support.

  **THE TWO PLANES MADE OPPOSITE CHOICES ON THE SAME QUESTION, and only one of them is
  measured.** `adce_enf_ctx_t` carries the comment "Per-thread. Never shared, so none of
  these fields is atomic" — the Enforcement Plane gives every ingress thread a private,
  non-atomic bucket and aggregates off the hot path, which is why
  `harness_concurrent` has to sum per-thread spans after `pthread_join` to write the
  aggregate identity down at all. The Observation Plane does the reverse: one
  `_Atomic uint64_t`, written by every ingress thread, on the arrival path. Same question,
  opposite answers, and the plane that chose sharing is the one with the measured ceiling.

  Neither choice was wrong when made — the Enforcement Plane needs per-thread state because a
  token bucket IS per-thread policy, and the Observation Plane needs one number because the
  statistic is global. What the measurement shows is that "the statistic is global" does not
  require "the counter is global".

  **CANDIDATE FIX, described so it can be argued with rather than to reserve the decision:**
  per-thread tap counters, summed at epoch close. `adce_obs_counter_take` is already called
  exactly once per epoch from inside `adce_obs_epoch_close`, on the observer thread, off the
  arrival path. Summing T counters there instead of exchanging one is O(T) work in a place
  that already runs off the hot path, once per `ADCE_OBS_EPOCH_NS` — 10 ms — against a
  per-arrival cost paid millions of times per second. The hot path loses its shared line
  entirely.

  **NOT IMPLEMENTED, and it must not be picked up as tidying.** Three reasons, each
  sufficient on its own.

  1. It changes `adce_obs_counter_t`, `adce_obs_tap` and `adce_obs_epoch_close` — a public
     type, the per-arrival API and the producer. That is an ARCHITECTURE change and the
     working agreement requires it to be proposed and waited on, not folded into a step whose
     stated scope was measurement.
  2. It is not evidence. It would move no boundary on the unverified list, so under the
     stopping rule it does not belong in the three-pull-request window.
  3. The design questions it opens are not answered here: how a consumer registers a thread's
     counter, what happens when threads outnumber a fixed array, whether the overrun identity
     `total_tapped == arrivals_closed + discarded + residual` survives T drains instead of
     one, and whether the harness's tap-before-gate assertions still read the same counter.
     None of those is hard; none of them is decided.

  **LANDED.** The stopping-rule window closed with #36, and this was item one as recorded.
  The context now owns `adce_obs_counter_t slots[ADCE_OBS_MAX_INGRESS]`; an ingress thread
  calls `adce_obs_claim_counter` once at start and taps through the returned pointer.
  `adce_obs_tap` KEPT ITS SIGNATURE, so the per-arrival path is the same relaxed
  `fetch_add` on the same padded type, now on a line nobody else writes.

  **The four questions the proposal listed as undecided, answered.**

  *How a consumer registers a thread's counter*: it does not register one, it CLAIMS one.
  The alternative — the caller owns the counter and hands it in, mirroring
  `adce_enf_ctx_t` — was rejected on LIFETIME, which is the axis the two planes actually
  differ on. An enforcement bucket dies with its thread and nothing needs it afterwards; an
  ingress thread's undrained arrivals still have to be accounted for after it exits. A
  caller-owned counter would leave the registry pointing at storage that may be gone, and
  no API can stop a caller putting one on its stack. A library-owned slot outlives the
  thread that claimed it.

  *What happens when threads outnumber a fixed array*: `adce_obs_claim_counter` returns
  NULL and the caller MUST NOT run that ingress thread. No fallback to a shared counter —
  that would be arithmetically correct and would silently restore the contention the change
  removes, under load, at the moment it costs most. Nor may the thread skip tapping:
  arrivals invisible to the detector make pressure read LOW and the gate shed LESS, which
  is fail-OPEN on the one axis this plane may never fail open on.

  *Whether the overrun identity survives T drains*: it does, and it is the ONE ASSERTION
  WHOSE SHAPE CHANGED. `residual` in `total_tapped == arrivals_closed + discarded +
  residual` was a single atomic load and is now a sum over claimed slots. Exact, because
  each arrival increments exactly one slot and each slot is drained into exactly one
  accumulator. What the T exchanges give up is the epoch boundary being a single INSTANT —
  an arrival landing between the first exchange and the last is attributed to the NEXT
  epoch, never dropped.

  *Whether the harness's tap-before-gate assertions still read the same counter*: they do
  not, and they must not. Each ingress thread claims its own slot; the assertions read the
  sum.

  **`ADCE_OBS_MAX_INGRESS` IS A DEPLOYMENT PARAMETER, NOT A DERIVED BOUND.** Nothing
  computes 64. It is a capacity chosen to exceed any ingress pool this library has been
  pointed at, and a deployment with more threads must raise it. The contrast that makes the
  distinction concrete is `ADCE_OBS_WINDOW_N`, whose ceiling of 125 IS derived — from
  `sup z` and `z_hi` — and is pinned by an assert that RECOMPUTES it. This one follows from
  nobody's arithmetic, and the range assert guarding it is likewise a sanity bound rather
  than a derivation. Dressing it as derived would be the error this document records
  against the `0.067` repeat count: a number with no basis presented as though it had one.

  It costs SPACE and never TIME, because the drain iterates CLAIMED slots. At 64 on a
  128-byte line `sizeof(adce_obs_ctx_t)` is 8448 bytes.

  **THE ACCEPTANCE TARGET WAS PRE-REGISTERED AND WAS MET.** The target was the PRIVATE arm
  of `bench/tap_contention.c` — 1.74 / 1.77 / 1.93 / 2.13 ns at T = 1/2/4/8 on the
  development host. A third arm, SLOTS, taps through a real `adce_obs_ctx_t`. Median ns per
  arrival, 10 runs, n per cell is runs x T:

  | arm | T=1 | T=2 | T=4 | T=8 |
  |---|---|---|---|---|
  | shared (overturned) | 1.78 | 6.55 | 19.65 | 123.89 |
  | private (control) | 1.82 | 1.80 | 1.93 | 2.17 |
  | **SLOTS (shipped)** | **1.79** | **1.81** | **1.93** | **2.20** |
  | target | 1.74 | 1.77 | 1.93 | 2.13 |

  Within 3% of target at every T, and within the control's own spread — the PRIVATE arm
  re-measured today reads 1.82 against its own 1.74 from #33, a larger gap than the one
  between PRIVATE and SLOTS. Library ownership of the slots costs nothing measurable.
  Aggregate throughput went from 562 → 62 M arrivals/s across T = 1 → 8 to 558 → 3236.

  **BOTH CI ARCHITECTURES, and the target does NOT transfer to them as an absolute.** The
  1.74/1.77/1.93/2.13 figures are development-host numbers, and the CI hosts are slower at
  this operation by their own control arm: PRIVATE reads 3.03 ns at T=1 on x86_64 CI and
  4.18 on arm64 CI. Comparing SLOTS against a dev-host absolute would therefore report a
  "gap" that is the machine. The comparison that IS valid is SLOTS against the PRIVATE
  control measured in the same run on the same host, which is what the third arm exists for.
  2,000,000 arrivals per thread, 5 runs, n per cell is runs x T:

  | host | arm | T=1 | T=2 | T=4 | T=8 |
  |---|---|---|---|---|---|
  | `ubuntu-24.04` x86_64, 4 vCPU | shared | 2.99 | 25.35 | 56.03 | 105.60 |
  | | private | 3.03 | 2.99 | 2.43 | 2.81 |
  | | **SLOTS** | **3.01** | **2.99** | **2.33** | **3.27** |
  | | SLOTS/private | 0.99x | 1.00x | 0.96x | **1.16x** |
  | `ubuntu-24.04-arm` aarch64, 4 vCPU | shared | 4.17 | 13.58 | 27.18 | 53.73 |
  | | private | 4.18 | 4.17 | 4.19 | 5.67 |
  | | **SLOTS** | **4.17** | **4.16** | **4.17** | **6.99** |
  | | SLOTS/private | 1.00x | 1.00x | 1.00x | **1.23x** |

  At T = 1, 2 and 4 the shipped arm is indistinguishable from the control on both hosts.
  **At T = 8 it is not, and that is reported rather than absorbed:** 1.16x on x86_64 and
  1.23x on arm64. Both runners are 4 vCPU, so T = 8 is oversubscribed two to one, and the
  MINIMA in those cells are identical between the arms — 2.31 against 2.31 on x86_64, 4.17
  against 4.16 on arm64 — which places the difference in the outliers rather than in a
  per-tap cost. That is an observation about where the difference lives, NOT an
  attribution: nothing here separates "SLOTS is descheduled slightly more often" from "the
  contiguous slots interact with the runner's cache differently under oversubscription",
  and with n = 40 per cell and one run of the job, it should not be quoted as a rate. The
  aggregate numbers show the change working regardless — x86_64 goes from 334 → 68 M
  arrivals/s across T = 1 → 8 on the shared arm to 332 → 1592 on SLOTS, and arm64 from
  240 → 134 to 240 → 872.

  **The cadence prediction holds on both CI hosts more cleanly than on the laptop**, which
  is what the job's own comment predicted for the reason it gave: the quantity is a
  sleeping thread's wakeup jitter, and a uniform-core runner is the better instrument than
  a machine whose effective concurrency this document records as not known. 200 intervals
  per arm-run, 3 runs. Every median is within about 90 ns of the nominal 10 ms at every
  slot count on both hosts, and `late` is zero everywhere except a single 4-slot run on
  x86_64. On arm64 the 64-slot arm has the SMALLEST tail of any arm — max 10,009,864 ns
  against the 1-slot arm's 10,084,576.

  **What is still not settled is what #33 already said was not settled**, and landing the
  fix does not close it: the benchmark loop does nothing but tap, so it is the worst case
  for line migration, and a real ingress site interleaves the gate, the clock and request
  work. The SIGN was never in doubt and is now acted on; the MAGNITUDE at a realistic duty
  cycle remains untested.

- **THE SUMMED IDENTITY IS BLIND TO A SKIPPED SLOT, AND A WHOLE INGRESS THREAD CAN GO
  UNCOUNTED WITH THE SUITE GREEN.** Found by mutation while proving the redefinition above,
  and it is a property of the new design rather than of the code that implements it.

  `total_tapped == arrivals_closed + discarded + residual` is a CONSERVATION statement: it
  asks whether arrivals were lost. A drain that skips a slot loses nothing — the arrivals
  stay in the slot and simply move from `arrivals_closed` to `residual`, both sides
  together — so the identity holds exactly while one thread's arrivals never reach the
  statistic at all. Measured, not argued:

  | mutation to `adce_obs_drain` | identity sites fired | suite |
  |---|---|---|
  | counts slot 0 but does NOT zero it | **2 of 2** | RED |
  | SKIPS slot 0 entirely | 0 of 2 | red, unrelated cases |
  | SKIPS slot 3 (only the 4-thread rig reaches it) | 0 of 2 | **GREEN, exit 0** |

  The mandated mutation goes red at both sites, so the sum does the work the single load
  did. The third row is the finding: with the index chosen so that no single-slot test
  covers it, the ENTIRE SUITE passed while a quarter of the ingress went uncounted. That
  direction is fail-OPEN — fewer arrivals seen means lower pressure means less shedding.

  **THE GAP IS NOT NEW. ITS FAILURE MODE IS, AND THAT IS THE WHOLE FINDING.** Two readings
  are available and both are wrong. "This change introduced the gap" is wrong: the identity
  was conservation-only before, and a drain that did not drain would have satisfied it then
  too. "The gap pre-existed, so nothing changed" is wrong in the more dangerous direction.

  What changed is DETECTABILITY. With one shared counter, skipping the drain meant skipping
  the ONLY drain: `arrivals_closed` stays at zero, observed throughput falls to zero, the
  detector sees nothing at all, and the failure is LOUD — several cases fire and no
  deployment could run that way for a second. With T slots, skipping one leaves seven
  eighths of the ingress flowing. The statistic still moves, pressure still tracks, epochs
  still publish, and the suite still exits 0. **The defect went from impossible to miss to
  impossible to see, and the redesign changed that without touching a line of the
  assertion.**

  **The axis is the one R4 named, and it is the same axis in both places.** A thread whose
  arrivals are never drained is a thread the detector does not count. The measured rate
  reads low, so `d` reads low, so pressure reads low, so `adce_enf_should_shed` sheds LESS
  — fail-OPEN on the containment axis, which is the single direction this plane may never
  fail. R4 reached that conclusion for a thread that cannot CLAIM a counter and must
  therefore not run; this reaches it for a thread that claimed one and is silently not
  drained. Same undercount, same sign, same consequence; the difference is only that the
  first is refused at startup and the second is invisible at runtime. That is why the
  refusal in `adce_obs_claim_counter` is not enough on its own and the drain needed a gate
  of its own.

  **M4 — THE NEW CASES TESTED IN THE OVER-DIRECTION, so they do not repeat at a finer grain
  the one-sidedness they were written to fix.** An assertion that catches only undercounting
  would be the same defect one level down.

  | mutation | suite | cases that fire |
  |---|---|---|
  | (i-a) a slot DRAINED twice — `take()` called twice | GREEN | none — **and correctly so, see below** |
  | (i-b) a slot COUNTED twice — load, then take | RED | **6**: both new cases, both identity sites, `obs_ewma_update`, `obs_sigma_floor` |
  | (ii) claim hands the SAME slot to two threads | RED | **1**: `obs_claim_capacity` alone |

  **(i) and (ii) ARE DIFFERENT MUTATIONS, and the answer decides whether one case would
  have sufficed. It would not.** They sit in different functions — the drain and the claim —
  and they break different properties. (i-b) is an ARITHMETIC defect and the identity is
  fully two-sided against it: double-counting inflates the drained total against an
  unchanged residual, so conservation breaks immediately and six cases fire. (ii) is an
  EXCLUSIVITY defect and is arithmetically INVISIBLE — two threads sharing a slot still
  increment atomically, no arrival is lost, and every conservation statement in the suite
  stays exactly true. What it destroys is the thing the change exists for: the two threads
  are back on one cache line, which is true sharing restored under a green suite. Exactly
  one assertion in the repository catches it, the pairwise pointer-distinctness check in
  `obs_claim_capacity`, and that is why the check is written as distinctness rather than as
  a non-NULL test.

  **Why (i-a) going green is inertness and not a blind spot, checked rather than reasoned.**
  `adce_obs_counter_take` is an atomic EXCHANGE, so a second take on the same slot returns
  zero: "drained twice" and "counted twice" are not the same event, and only the second is a
  defect. Run directly — 100 taps, then `take` returns 100 and the second `take` returns 0.
  A doubled drain adds nothing, so there is nothing for a case to catch, and a red here
  would have meant a case asserting over a quantity the design does not produce. Recorded
  because a green run is normally weak evidence, and the only thing that makes it strong
  here is the separate demonstration that the mutation is a no-op.

  **CLOSED BY BRANCH 3, not by a note.** `obs_drain_covers_claimed` claims four slots and
  taps them with distinct powers of ten, so the drained sum NAMES which slots were visited;
  it then asserts the residual is zero, so the drain zeroed what it counted. Deterministic,
  single-threaded, no timing. `obs_claim_capacity` asserts that all
  `ADCE_OBS_MAX_INGRESS` claims succeed and are pairwise DISTINCT — a claim handing the
  same slot to two threads satisfies "non-NULL" while recreating true sharing on that line
  — and that three further claims each return NULL. Both run in all three `verify.sh`
  profiles.

  **P3 IS UNENFORCED AND CANNOT BE ENFORCED BY THIS SUITE, stated rather than assumed.**
  Mutating the drain to iterate `ADCE_OBS_MAX_INGRESS` instead of `claimed` leaves every
  test green, because draining unclaimed slots adds zero. That the drain is O(claimed) is a
  COST property, and no assertion over answers can see a cost. It is maintained by the code
  reading `claimed` and by nothing else.

- **THE CADENCE PREDICTION HELD, AND THE PROTOCOL THAT TESTED IT HAD A CONFOUND ITS AUTHOR
  DID NOT ANTICIPATE.** `bench/drain_cadence.c`, pre-registered numbers-free before any
  measurement; `git log` on that path is the proof of order.

  The question: closing an epoch now costs one exchange per claimed slot instead of one
  total. The prediction: no perturbation resolvable, on a standing bound of 39 ns for 64
  uncontended exchanges against a 10 ms epoch — a duty cycle near 4e-6. Registered
  refutation criteria were the median interval rising with the slot count beyond the
  within-arm spread, `late` rising with it, or the tail widening with it.

  300 intervals per arm-run, 20 runs per arm, development host:

  | slots | median of medians (ns) | within-arm spread | p99 median | late |
  |---|---|---|---|---|
  | 1 | 10,000,625 | 5,875 | 10,035,958 | 6 |
  | 4 | 10,000,542 | 20,583 | 10,038,688 | 13 |
  | 16 | 10,000,417 | 1,376 | 10,034,188 | 6 |
  | 64 | 10,000,334 | 959 | 10,033,479 | 8 |

  The across-arm span of medians is **291 ns** over both orderings against a within-arm
  spread reaching 26 us. Medians do not rise with the slot count; 64 slots has the tightest
  spread and the lowest p99. No criterion refuted.

  **The instructive part is the trend that was not there.** At 5 runs per arm `late` read
  **0 / 3 / 3 / 6** across slot counts 1 / 4 / 16 / 64 — monotone, and one of the three
  registered refutation criteria. Two objections were available and only one of them is
  worth anything.

  The weaker is magnitude: `ADCE_OBS_THREAD_LATE_NS` is 1 ms and the drain's bound is 39 ns,
  four orders too small to make an epoch late. That reasoning is correct and it is exactly
  what this document elsewhere calls explaining a number away rather than testing it.

  The decisive one is a CONFOUND IN THE PROTOCOL: the arms run in BLOCKS, so slot count is
  perfectly confounded with wall-clock position, and the late events sat in the blocks that
  ran last. The discriminating experiment cost one `sed` and eight minutes — a scratch
  build with the arm order REVERSED, run alongside the original at 20 runs per arm:

  | order | late by slot count 1 / 4 / 16 / 64 |
  |---|---|
  | forward (1, 4, 16, 64) | 6 / 13 / 6 / 8 |
  | reversed (64, 16, 4, 1) | 6 / 14 / 4 / 3 |

  The monotone trend does not survive n = 20 in either order; reversed, the 64-slot arm has
  HALF the late epochs of the 1-slot arm. **This document carries five entries about one
  observation quoted as a rate, and 0/3/3/6 was very nearly the sixth.** What survives
  unexplained is that the 4-slot arm is worst in BOTH orderings, which is neither a
  slot-count nor a position effect; it is recorded with its n and not fitted to a story.

  The reversed-order control is deliberately NOT committed. The confound is a defect in the
  protocol's design and belongs in its record as one, rather than being quietly removed by
  interleaving the arms after the fact and leaving no trace that the first reading was
  wrong.

- **THE ENTROPY SYSCALL, EXECUTED AND GATED. The deliverable is a CONTROL, not a
  measurement, and the entry it retires was wrong about its own gap.** Entry (3) of the
  unverified list.

  **What the gap actually was.** The list said the Darwin arm "has no automated coverage at
  all" and that it is "the only entry where a whole code path is unexecuted by any automated
  gate". The second half is false and was false when written. `adce_rng_seed` calls
  `ADCE_GET_ENTROPY` on first use of the RNG, so every local gate execution on Darwin has
  always entered that function. Re-derived from the source rather than from the list, which
  is the only reason it surfaced.

  **What was genuinely unreached is narrower and worse.** The only shipping caller asks for
  `sizeof(buf) == 16` bytes. 16 <= 256, so the clamp `if (chunk > 256U) chunk = 256U;` has
  never been taken by any caller on any host, and the loop has never iterated more than once —
  in a function whose own comment states that "the chunking loop is mandatory, not defensive".
  The code that exists solely to handle the 256-byte limit was the code nothing ran.

  **Why this is a control where the tap was a measurement.** The tap's contended cost is a
  property of the host's cache coherence: a threshold on it would be a band with no
  derivation, so it went to branch 2 with its n. This one is BINARY — the syscall either
  fills the request or it does not — so it is assertable, and branch 1 applies. That is the
  distinction the two entries turn on, and it is why one produced numbers and the other
  produced a gate.

  **`test_platform_entropy`**, in `test/t_adce_platform.c`, running in all three `verify.sh`
  profiles. Six lengths straddling the limit — 16, 255, 256, 257, 512, 1000 — where 257 is the
  smallest input that forces a second chunk. Four properties, and only the first is a smoke
  test:

  | # | property | what it catches |
  |---|---|---|
  | 1 | the call reports success | the syscall failing outright |
  | 2 | every byte inside the request is written, checked PER 256-BYTE REGION | a loop that fills one chunk and stops — a whole-buffer check would pass on the first chunk alone |
  | 3 | no byte past the request is touched | an off-by-one in the chunk arithmetic |
  | 4 | two successive fills differ | a stub returning 0 without writing |

  **The one assumption, stated rather than buried.** "Written" is inferred from a poison byte
  being gone, and genuine entropy can produce the poison value. Per 16-byte region that is
  `256^-16`, about 1e-39, and no region checked is smaller than 16 bytes. It is a
  probabilistic argument and the case says so in its own comment.

  **PLATFORM-AGNOSTIC BY CONSTRUCTION.** The case contains zero preprocessor conditionals, so
  it exercises whichever arm the host compiled: real chunking on Darwin, `getrandom` at the
  same sizes on Linux. **Not claimed and still uncovered:** the Linux arm's `EINTR` and
  short-read retries, which need a signal to provoke. Entry (3) is retired for the chunking,
  not for that.

  **MUTATION-PROVED THREE WAYS**, each producing an attributing message rather than a bare
  assertion number: chunking stopped after one chunk gives
  `left bytes [256,512) entirely unwritten`; one byte past the request gives
  `wrote past the request at offset 257 (0xFF, expected 0x5A)`; and a stub returning success
  without writing gives `left bytes [0,16) entirely unwritten`.

  **One mutation had to be narrowed, and that is recorded because it is instructive rather
  than tidy.** An unconfined overrun — one byte past on EVERY call — smashes `adce_rng_seed`'s
  16-byte stack buffer and the stack protector aborts at exit 134 before this case ever runs.
  That is a real detection, by a different mechanism, and it would have let this case take
  credit for a catch that was not its own. The mutation was confined to `len > 256` until the
  case is demonstrably the thing that catches it.

  **No shipping translation unit changed.** `include/` and `src/` are byte-identical to
  `main`, so the Linux arm cannot have been affected and R4's propose-and-wait never arises.

- **THE SEQLOCK SPIN: MEASURED, ASSERTED, AND THE PRE-REGISTERED PREDICTION REFUTED ON ONE
  HOST OF THREE.** Entry (4), retired. Branch 3 was reachable, so this produced a CONTROL as
  well as numbers — and the control is host-independent where the numbers are not.

  **Two premises corrected before any measurement.** The analytic estimate entry (4) cites
  exists — `docs/enforcement-plane.md` §2, "the odds of landing inside a write on the order
  of the write's duration divided by 10 ms". And `adce_epoch_read` has **no retry loop**: zero
  `while` statements, and on a torn read it RETURNS 0 rather than reading again. The only loop
  in the entire read path is the entry spin in `adce_seqlock_read_begin`. "The retry path" is
  one thing, and it is the spin. The torn branch was excluded with that reason stated.

  **THE STRUCTURAL FINDING, which is worth more than the timing.** SUPERSEDED as of the
  hardening recorded further below — the predicate now rejects an odd start, so the spin is one
  of two guards rather than the sole one. Kept as written because it is what the measurement
  found, and because the hardening is only intelligible against it.
  At the time, `adce_seqlock_read_retry`
  never checked the parity of the value the reader started from:

      return s != start;

  So a reader that begins inside a write AND finishes inside the same write sees an unchanged
  sequence, concludes nothing moved, and **accepts a payload the writer had not finished
  assembling.** Nothing downstream catches it. **The entry spin is therefore the SOLE guard
  against consuming a mid-write state, not an optimisation over the retry predicate.**

  This was found by being wrong. A no-spin probe was built as an anti-vacuity guard, expected
  to FAIL and thereby prove the window was open. It succeeded, and the reason was the missing
  parity check.

  **BRANCH 3 REACHED: `test_seqlock_retry_constructed`**, running in all three `verify.sh`
  profiles with no timing threshold anywhere. The writer opens the window by hand and leaves
  the payload half assembled — `epoch_id` advanced to 2 while `pressure` still holds the
  previous committed value, so `(0, 2)` is a pair no committed state ever carries. Four
  equalities:

  | # | assertion | what it establishes |
  |---|---|---|
  | 1 | the probe observed an ODD sequence | the window was open, by direct observation rather than by timing |
  | 2 | the probe SUCCEEDED without the spin | the finding above |
  | 3 | it accepted `(epoch=2, pressure=0)` | a state the writer never committed |
  | 4 | the real read returned the COMPLETE post-write state | the spin waited |

  Remove the spin and 4 becomes 2. The mutation is not described, it is the probe, and it runs
  on every gate execution.

  **FREQUENCY — a property of the schedule.** Development host, 10 runs per cell, ~2e9 reads
  per cell at one reader and ~5e9 at four:

  | writer cadence | 1 reader | 4 readers |
  |---|---|---|
  | 10 ms (shipped) | **0 in 2.04e9 reads** | 2.95e-9 per read |
  | 100 us | **0 in 1.93e9 reads** | 8.92e-9 |
  | 10 us | 5.25e-10 | 6.95e-8 |
  | 1 us | 3.39e-9 | 2.09e-8 |

  At the shipped cadence a single reader entered the spin **zero times in two billion reads**.
  A run that sees none measures nothing, which is why the cadence was swept down.

  **Both CI hosts are RARER still**, which strengthens the negligibility conclusion rather
  than weakening it. At the shipped cadence with four readers: 7.65e-9 per read on x86_64 CI
  (2 entries in 2.6e8 reads) and **0 in 2.0e8 reads** on arm64 CI. Across the entire sweep
  arm64 CI recorded one entry in 1.0e9 reads.

  **DISTRIBUTION — Model A is refuted ON THE DEVELOPMENT HOST and NOT CORROBORATED ANYWHERE
  ELSE.** The qualification is not hedging; it is the whole result, and stating the dev-host
  half alone would be this project's own recurring error.

  Model A was predicted: independent Bernoulli entry, geometric iterations, thin tail. The
  development host's spin-iteration histogram at four readers refutes it outright:

  | M3, 4 readers | entries | 1 | 2 | 3 | 4-7 | 8-15 | **16+** |
  |---|---|---|---|---|---|---|---|
  | cadence 10 us | 328 | 14 | 6 | 2 | 41 | 48 | **217 (66%)** |
  | cadence 1 us | 82 | 1 | | | 1 | 3 | **77 (94%)** |

  **Neither CI host reproduces that tail, and one of them is the authoritative platform.**
  Same binary, same sweep, 3 runs of 100 ms per cell against the dev host's 10 of 300 ms:

  | host | total entries, whole sweep | reads | largest bucket seen |
  |---|---|---|---|
  | M3 Darwin arm64, 8 core | 417 | 2.4e10 | **16+**, 294 of them |
  | `ubuntu-24.04` x86_64, 4 vCPU | 70 | 1.5e9 | **4-7**, four of them |
  | `ubuntu-24.04-arm` aarch64, 4 vCPU | **1** | 1.0e9 | one entry, in 16+ |

  On x86_64 CI the tail is THIN — 1, 2 and 3 iterations dominate and nothing reaches 8. On
  arm64 CI the sweep produced ONE entry in a billion reads, which is not a distribution at
  all. **So the refutation of Model A rests on one host, and the arm64 evidence this project
  ranks highest neither supports nor contradicts it, for want of events.**

  **The mechanism is a HYPOTHESIS and is not tested.** The reading that fits the dev-host
  data is that spinning readers hammer the sequence line with acquire loads while the writer
  needs it exclusive, so a reader that enters the spin DELAYS the writer it waits for. That
  would be a coupling Model A assumed absent, created by the spin itself. But a second
  reading fits equally well and this document already supplies it: the M3 is 4 performance
  plus 4 efficiency cores, macOS migrates threads between clusters by QoS, this project sets
  no QoS, and **the effective concurrency of the local gate is recorded here as NOT KNOWN.**
  A reader parked on an efficiency core while the writer runs on a performance one would
  produce long spins for a reason that has nothing to do with coherence. Nothing measured
  here separates the two, and the CI hosts — uniform cores, hard `nproc` bound — are exactly
  where the cluster explanation predicts the tail should vanish, which is what happened.

  Recorded as unresolved rather than resolved. Separating them needs either an affinity API
  the development machine does not expose, or a CI host with enough events to characterise a
  distribution, and neither is available.

  **COST.** A spin ends when the writer finishes, so the write window bounds it.
  `adce_epoch_publish`, uncontended, n = 200,000 calls per measurement:

  | host | ns per call |
  |---|---|
  | M3 Darwin arm64 | 6.8, 6.9, 7.6, 8.1, 10.9 across 5 runs |
  | `ubuntu-24.04` x86_64 | 1.354, one run |
  | `ubuntu-24.04-arm` aarch64 | 5.062, one run |

  The two CI figures are single runs and are labelled as such. The first version of this
  measurement reported 0.000 ns because the state object never escaped and the compiler
  deleted the loop; it is now static and read back, which is recorded because a measurement
  that silently measures nothing is this project's documented failure mode.

  **DECISION-RELEVANCE — the verdict the brief asked for, against the tap.** The tap costs
  1.74 ns per arrival at one thread and 134 ns at eight on this host. The spin contributes
  frequency times cost: at the shipped cadence with four readers that is about 3e-9 entries
  per read, and even a 16-plus-iteration spin is tens to hundreds of nanoseconds. The expected
  per-arrival contribution is therefore on the order of 1e-6 ns — **six to eight orders of
  magnitude below the contention term the tap measurement already found.**

  **It changes no decision the tap has not already decided, and that is a complete result
  rather than a weak one.** The fat tail does not rescue it: a tail event costing a few
  hundred nanoseconds, once per roughly three hundred million reads, is not a term any
  deployment sizes against. Entry (4) is retired on that basis and not on a null.

- **THE STOPPING RULE, in force from 2026-09-11.** The next three MERGED pull requests move
  the library's evidence boundary. No exceptions, including "small" documentation corrections
  and including defects found in the apparatus, which are FILED in the list rather than
  fixed. The sole exception is a defect that renders a REQUIRED check inert — required
  meaning the three ruleset contexts, `sanitizers (ubuntu-24.04)`,
  `sanitizers (ubuntu-24.04-arm)` and `shipping-target` — because that invalidates the
  evidence the three are producing. A defect in `boundary-note` or `internal-use` does not
  qualify; both are advisory.

  **If a fourth apparatus pull request merges before three evidence ones do, record that the
  stopping rule failed and treat the failure as the finding.**

  Count: this pull request is the THIRD AND LAST. The window is closed. Entry (1) was retired
  by #33, entry (3) by #34 and entry (4) by this one; #35 was the admitted exception, a
  defect that made a required check report a false red, and it did not consume a slot.

  **What the window cost and what it bought, so the next one can be sized.** Three evidence
  pull requests retired three list entries and produced two controls and one measurement.
  Every one of the three found its entry's own premise to be WRONG — the tap's entry
  understated a global ceiling, entry (3) claimed a path was unexecuted when only its chunking
  was, and entry (4) said an analytic estimate was missing when it exists and called a
  non-existent retry loop "the retry path". That is three for three, and it is the strongest
  argument in this document for re-deriving from source rather than reading the list.

  **Apparatus work is now unblocked**, and the filed items below are the queue. The
  pre-commit-on-main hook is first, for the reason recorded with it.

  **The rule has no gate behind it and is self-policed**, which is exactly the class this
  document condemns elsewhere. Nothing goes red on a fourth apparatus merge. That is stated
  rather than hidden, and the failure clause is the only enforcement there is.

  Recording the rule could not have its own pull request without consuming one of the three
  slots on its first move, which is why it rides in this commit. Filings work the same way:
  they accumulate and land with the next evidence pull request.

- **FILED, NOT FIXED, under the stopping rule.** Apparatus defects found and deliberately
  left alone. Items 3 and 4 came from the entropy pull request; item 4 was RESOLVED by #35,
  which is the stopping rule's admitted exception, and is kept with its resolution rather
  than deleted. Item 5 came from this one.

  **THE LIST WAS CLEARED ON 2026-09-14.** Status of every item at that point, so the next
  reader does not have to reconstruct it from six paragraphs:

  | # | what was filed | outcome |
  |---|---|---|
  | 1 | entry (2) of the unverified list stale since #16 | **FIXED** — entry (2) retired above, 8 days stale |
  | 2 | `boundary-note` selector blind to `bench/` | **FIXED** — selector derived, names no directory |
  | 3 | a commit attempted on `main`, twice | **FIXED (partially)** — `scripts/githooks/pre-commit`; a fresh clone is still unprotected, see below |
  | 4 | ran-tests guard split-line false red | RESOLVED by #35, already recorded |
  | 5 | the #35 NOTE asserts a splice its predicate does not establish | **FIXED** — the NOTE names the observation |
  | 6 | `ADCE_PUBLIC_NO_INTERNAL_USER` is a marker nothing reads | **RESOLVED 2026-09-14** by #26 merging |

  **Two items were carried in conversation that were never in this list**, and the difference
  is worth recording because a filing that lives only in a message is not filed at all:

  - *the exclusivity assertion is singular* — recorded in the M4 table when the per-thread
    counter landed, but never entered here as an apparatus item. Now marked in
    `test/t_adce_observe.c` at the loop itself, which is where a reader meets it.
  - *#26's fourth commit stretched its stated scope* — it carried the record AND a script
    guard for the missing-interpreter path. That was never written down anywhere in this
    repository; it existed only in a hand-off message. Recorded now as the item below.

  1. **Entry (2) of the unverified list is stale, and has been since #16.** It still reads
     "Runnable, untested, and the next task" for the aggregate ceiling under real concurrency.
     That landed in #16: `harness_concurrent` asserts `harness_bucket_identity` per thread and
     the two-sided aggregate identity. This is the FOURTH time the list has gone stale in the
     under-claiming direction, and the first time it has been left stale on purpose.
  2. **The `boundary-note` selector does not cover `bench/`.** The pattern is
     `^(src|include|test)/.*\.(c|h)$`, and this pull request creates `bench/tap_contention.c`,
     which it cannot see. Same shape as the `include/` hole repaired in #29 — a selector
     written from where the code was assumed to live — and found the same way, by adding code
     somewhere its author had not considered.

     **STILL OPEN, and now with a second instance.** The per-thread-counter pull request adds
     `bench/drain_cadence.c` and a third arm to `bench/tap_contention.c`, both invisible to
     the selector for the same reason. That pull request fires the job anyway, because it
     changes `src/` and `include/` and `test/` as well — which is the part worth noticing:
     the hole is not that `bench/` changes go unnoticed in general, it is that a pull request
     touching ONLY `bench/` would take the quiet branch, and this one gives no evidence
     either way. It remains filed rather than repaired because the repair is a gate change
     and belongs in its own pull request with its own mutation proof, per the rule that a new
     gate's first duty is to be shown failing on a change it must catch.
  3. **A commit on `main` was attempted, for the second time from the same root cause.** The
     session-start status line said the branch was `evidence-tap-contention`; by the time the
     write happened that branch had been merged and squashed, and the working branch was
     `main`. The line was trusted at write time instead of re-derived. A `git commit --amend`
     then rewrote `73eff8f`, the squashed merge of #33, locally.

     **The damage was contained by the ruleset, not by discipline**, and that distinction is
     the whole reason this is filed rather than shrugged off. `non_fast_forward` on `main`
     would have rejected the push; the reset that undid it was caught by reading `git log`
     after the fact, not before. Nothing reached the remote and `origin/main` was never
     touched, which was luck wearing a control's clothing.

     **The candidate control is a pre-commit hook refusing commits on `main`.**
     `scripts/hooks/` already exists and carries `post-edit-build.sh` and `stop-verify.sh`, so
     there is a home for it and a precedent for the mechanism. Its false-positive rate is
     structurally ZERO rather than merely low: the ruleset makes `main` unpushable by direct
     commit, so every piece of work in this repository is branch-based by construction and a
     commit on `main` is never the intended act. That is a better profile than either gate
     proposed so far — the boundary note fires on about two thirds of pull requests forever,
     and the internal-use check fires once per function.

     NOT BUILT, because it is apparatus and this is an evidence pull request. It is the
     candidate to reach for first once the window closes, ahead of the tap redesign, on the
     grounds that it costs nothing and this is the second instance.

  4. **The ran-tests guard breaks when stdout and stderr interleave, and it is a FALSE RED
     rather than a silent pass.** Surfaced by this pull request, created by neither it nor the
     change it carries.

     Both gates merge the two streams and then demand an exact whole-line match:

         2>&1 | tee "$log"                      # verify-linux-gcc.sh:232
         grep -qxF "TEST OK: $name" "$log"      # verify-linux-gcc.sh:191, verify.sh:120

     stdout through `tee` is block-buffered and stderr is unbuffered, so a stderr write can
     land inside a half-flushed stdout line. This document already records that the two
     streams cannot be ordered against each other — it is why the teeth banners were moved to
     stderr with a BEGIN/END fence. What was not noticed is that the guard's `-x` makes a
     SPLIT line indistinguishable from an ABSENT one.

     Observed on `shipping-target`, linux/amd64, run 34649... : the binary printed
     `All tests passed`, `loop_draw_invariance` ran and passed, and the log carries the
     orphan fragment `_draw_invariance` on its own line where `TEST OK: loop_draw_invariance`
     should be. The guard then reported `defines test_loop_draw_invariance, but it never ran`.
     Both the plain strict leg and the pinned leg failed the same way; arm64 and every
     `verify.sh` profile passed.

     **This pull request did not cause it and cannot have.** It adds one `printf` early in the
     run, which moves where the 4 KiB stdout buffer flushes; the interleaving it exposes has
     been latent since the guard was written. The defect is that a passing suite can be
     reported as an unrun test by an output artefact.

     **It is a false RED, not an inert check**, so it does NOT meet the stopping rule's sole
     exception, which covers a defect that renders a required check inert. It is filed here
     unfixed and it BLOCKS this pull request, because `shipping-target` is required. Stating
     that plainly is better than tuning anything until it goes green, which is the failure
     this project has an entry about.

     The candidate fixes, unranked because choosing between them is the fix and this is the
     filing: keep the two streams in separate files and run the guard against stdout alone;
     or relax `-qxF` to a line-anchored match tolerant of a leading fragment; or have the
     runner emit its manifest to a third descriptor that nothing else writes.

     **RESOLVED by #35**, the stopping rule's admitted exception, taking the first option:
     the three `2>&1` merges in `verify-linux-gcc.sh` are gone, `-qxF` is untouched, and both
     gates carry the invariant. Kept here with its resolution rather than deleted, because the
     filing is the record of what blocked an evidence pull request and why the exception was
     granted.

  5. **The #35 NOTE asserts a splice where its predicate only establishes a count
     disagreement.** Filed against text this project wrote three days ago, which is the point.

     The diagnostic added to both gates compares `TEST OK:` occurrences against well-formed
     whole lines and, when they differ, prints `output was SPLICED`. What the predicate
     actually establishes is narrower: **the two counts disagree.** Splicing is one cause. A
     legitimate line that happens to contain the token elsewhere is another, and M3 in that
     pull request fires the NOTE for exactly that reason — a substring inside other text, no
     splice anywhere. The NOTE was demonstrated firing on a case it misdescribes, and the
     demonstration was recorded as success.

     It is the printed-universal error in a new place: a predicate's OUTPUT is stated more
     strongly than the predicate supports. The verdict is unaffected — the NOTE never changes
     red to green — so this is a wording defect, not a control defect. The honest form names
     the observation and lists splicing as the leading explanation rather than the finding.

     NOT FIXED: this is an evidence pull request and the wording is apparatus.

  6. **`ADCE_PUBLIC_NO_INTERNAL_USER` IS AN ANNOTATION NOTHING READS.** The
     per-thread-counter change annotates `adce_obs_claim_counter` and `adce_obs_residual`
     with it, as a DECISION rather than as a red silenced after the fact — and it could not
     have been the latter, because there is no red to silence: the `internal-use` check is
     **PR #26, still open**, and no job in `.github/workflows/verify.yml` matches the string.

     Recorded because the annotation LOOKS like it is doing something. A reader meeting it in
     a header would reasonably infer a check behind it, and the whole content of this
     document's five-gate table is that a control which appears to act and does not is the
     failure mode this project ranks worst. This one is a step further out: not a gate that
     passes without checking, but a marker with no gate at all.

     The decision it records stands on its own merits and is stated in the commit rather than
     delegated to a checker: claiming a counter is the consumer's act exactly as tapping is,
     so the library reading these counters and never writing them means zero internal callers
     is the design working. What is not true is that anything enforces the annotation's
     spelling, placement, or continued accuracy.

     **RESOLVED 2026-09-14, when #26 merged.** The `internal-use` job is on `main` and reads
     the annotation; the marker is no longer a marker with no gate. On first contact the two
     annotations #38 had written blind both PASSED — parsed, reasons extracted, listed as
     declared rather than defects, with nothing adjusted to make that happen. That is n = 2
     and is recorded with its caveat in the internal-use entry above. A closed item is not
     carried forward; this one is kept with its resolution and its date, like item 4.

  7. **#26's fourth commit carried a script change alongside the record, which its own stated
     scope did not cover.** The commit is titled as the rebase record and it also added the
     missing-`python3` preflight to `scripts/check-internal-use.sh`.

     The instruction it was written under said to keep #26's three commits and add a fourth
     "only for the record items". The guard is not a record item — it is a gate change, and
     this document's own rule is that a gate change lands in its own commit with its own
     reason, precisely so it cannot ride along with something else.

     **It was flagged at the time rather than hidden**, and the reason it was done that way is
     still the reason: the alternative was amending #26's first commit, which would have
     rewritten the three commits that were the reviewed history of the proposal. Choosing
     between "the gate change rides with the record" and "the reviewed history is rewritten"
     is a real trade and neither option satisfies the rule.

     NOT FIXED, and not fixable retrospectively: #26 is merged and squashed, so there is no
     longer a fourth commit to split. What is recorded is the precedent — **a rebase that has
     to repair the thing it rebases needs its repair proposed as its own pull request, or the
     scope statement needs to admit the repair up front.** The third option, shipping a gate
     change inside a record commit because splitting it was awkward, is the one that was
     taken and should not be taken again.

- **THE PRE-COMMIT HOOK EXISTS AND A FRESH CLONE DOES NOT HAVE IT.** `scripts/githooks/pre-commit`
  refuses a commit made while `HEAD` is on `main`, naming the branch and leaving the staged
  changes intact. Built because the mistake occurred twice, both times from trusting a
  session-start status line instead of re-deriving the branch.

  **What it buys is narrower than "protects main".** The ruleset already makes a direct push
  impossible — GH013 — so a commit on `main` could never have landed. What it can do is sit on
  top of a squashed merge where an `--amend` rewrites it, which is exactly what happened to
  `73eff8f`. The hook moves the refusal from push time to write time. The `--amend` case was
  mutation-proved specifically, since that is the shape the second instance took.

  **The false-positive claim was checked and is narrower than filed.** The filing said the rate
  is "structurally ZERO". Of 80 commits on `main`, 34 end in `(#N)` and 46 do not; all 46 are
  dated 2026-09-04 or earlier, and every commit from 2026-09-06 on is a squash merge. So it is
  zero against the practice in force since the ruleset, over 34 merges, and the practice
  changed once.

  **THE PRECEDENT THE FILING CITED WAS THE WRONG MECHANISM.** It said `scripts/hooks/` "carries
  `post-edit-build.sh` and `stop-verify.sh`, so there is a home for it and a precedent for the
  mechanism". Those are CLAUDE CODE AGENT hooks wired through `.claude/settings.json`. Git never
  runs them and they would not see a commit typed at a terminal. `.git/hooks` held only samples
  and `core.hooksPath` was unset. The new hook lives in `scripts/githooks/` so the two
  mechanisms are not conflated.

  **A FRESH CLONE IS UNPROTECTED, AND THAT IS THE FINDING, not a caveat.** Measured in a real
  clone before any install step: `core.hooksPath` unset, zero active hooks, and a commit on
  `main` SUCCEEDED. `.git/hooks` is inside `.git` and cannot be tracked, so a committed hook
  cannot install itself. `core.hooksPath` does close it —
  `git config core.hooksPath scripts/githooks`, one command, after which the refusal works —
  but running that command is itself the manual step, and an install script only moves the
  manual step into a different command. **There is no arrangement in which cloning this
  repository gives you this control.**

  This project ranks a control that arrives only by manual setup below one the gate runs, for
  the same reason a silently-skipping profile is worse than one that does not exist: a clone
  without the config is indistinguishable from a clone with it until someone commits on `main`
  and nothing happens. It is kept because the alternative at the moment of the mistake is
  nothing, and because the mistake has occurred twice. It must not be described as protecting
  `main`.

- **`ADCE_TEST_ASSERT(seen[i] != seen[j])` IS THE ONLY CHECK OF SLOT EXCLUSIVITY IN THIS
  REPOSITORY, and the violation it catches is arithmetically invisible.** Marked at the loop
  itself in `test/t_adce_observe.c` as well as here, because the fact was previously recorded
  only in this document while the assertion it describes sat in a file that did not mention it.

  Measured, not assumed: mutation M4(ii) made `adce_obs_claim_counter` hand out a duplicate
  slot and ran the whole suite. Exactly one case failed. Two threads sharing a slot still
  increment it atomically, so no arrival is lost and
  `total_tapped == arrivals_closed + discarded + residual` stays exactly true at both sites.
  What is destroyed is the only thing the per-thread counter design exists for: the two threads
  are back on one cache line — the true sharing #33 measured at 12x to 77x — under a green
  suite.

  **NO SECOND ASSERTION WAS ADDED.** A duplicate of the same check is not independent evidence,
  and this document already has an entry on a green never seen failing. What protects the
  property is that the next person to touch `adce_obs_claim_counter` knows this line is the
  only guard, which is why the fact is written at the loop rather than defended with a copy.

- **`adce_seqlock_read_retry` NOW REJECTS AN ODD START, and the defect class is the INVERSE
  of the `adce_epoch_is_stale` one.** Approved as option 1 of four and implemented; the three
  rejected options are recorded with their reasons so the ruling is explicit rather than
  inherited.

      -    return s != start;
      +    return (start & 1U) || s != start;

  **THE CLASS, and it is the mirror image of the one recorded above.** With
  `adce_epoch_is_stale` the PROSE was right about the function and wrong about the system:
  every word described the function correctly while the gate had stopped going through it.
  Here the inverse. **The CODE was right and the primitive's precondition was unwritten at
  the point that depended on it.** `adce_seqlock_read_begin` spins while the sequence is odd
  and therefore cannot return one, so the composed pair was safe — but the safety property
  lived entirely in one function's spin loop, and `adce_seqlock_read_retry` neither stated nor
  checked the condition its correctness rested on. A partial function with an unwritten
  domain, called correctly by luck of who calls it.

  The two together are worth more than either: one says a true sentence can describe the
  wrong route, the other says correct code can rest on an unstated precondition. Neither is
  reachable by reading the function alone, which is what makes them a pair.

  **THE OPTIONS, AND WHY THE OTHER THREE WERE REJECTED.**

  | | option | rejected because |
  |---|---|---|
  | 1 | `(start & 1U) \|\| s != start` | **TAKEN** |
  | 2 | assertion active only under sanitizers | protects the build nobody ships; a check absent from the shipped configuration is the silently-skipping class this document ranks worst |
  | 3 | documented contract, no enforcement | **this was the state already, minus the documentation.** Choosing it means writing the invariant down and calling that a fix |
  | 4 | opaque `start` type only `read_begin` can produce | DEFERRED, not closed — see below |

  Option 1 was taken because the failure mode is accepting a half-assembled payload, a
  fail-OPEN on a fail-closed plane, against a cost the instrument could not resolve. The
  asymmetry decides it, not the arithmetic.

  **OPTION 4 IS DEFERRED AND NOT CLOSED, and this sentence exists so a future reader knows it
  was weighed rather than missed.** Making `start` an opaque type that only
  `adce_seqlock_read_begin` can produce would make the invariant structural instead of
  checked — strictly stronger. It was deferred because it changes BOTH signatures for a defect
  with no risky caller today, and because it would still not stop a future fast path
  constructing one deliberately. **What reopens it:** a second caller obtaining `start` by any
  route other than `read_begin`, or a non-spinning fast path added to the read side. Either
  makes the type the right answer and the branch a patch over it.

  **COMPILER SCOPE.** The instruction-count figures from the proposal are WITHDRAWN rather
  than restated. They were clang output on arm64, there is no local x86_64 or GCC to check
  them against, and one compiler's codegen must not read as a property of the shipped binary —
  the gate is GCC 14 on two architectures. Only the timing claim survives, with its scope on
  its face: clang on arm64 Darwin, uncontended, 20,000,000 calls per run over 7 runs, both
  forms converging to ~0.69 ns and differing by ~0.003 ns against a within-binary warm-up
  spread from 1.66 to 0.69. **No measurement of any kind was taken under GCC or on x86_64.**

  **MEASUREMENT REGIME, and why uncontended is the conservative side rather than the
  convenient one.** Under contention the added test is still perfectly predicted, because
  `adce_seqlock_read_begin` cannot return an odd value and the branch is not-taken on every
  reachable path. And the arrival path is dominated by the tap's measured 1.74–134 ns
  contention term, six to eight orders of magnitude above the term being added. That is the
  same decision-relevance test that retired entry (4), applied to a change rather than to a
  measurement.

  **THE PROOF IS STRUCTURAL AND THE SWEEP CONFIRMS IT**, in that order, because a sweep over a
  constructed range is a sampled claim wearing an exhaustive label. When `start & 1U` is 0 the
  hardened form short-circuits to `s != start` by the definition of `||`, for every `s` — so
  the two forms are identical on every even start over the whole of `uint64_t`. The sweep then
  confirms it over 7,340,041 comparisons with zero mismatches, stated as **identical over
  [0, 2^21)** and never as "no existing case changes".

  **THE TEST THAT DOCUMENTED THE DEFECT NOW ASSERTS THE FIX.**
  `test_seqlock_retry_constructed` carried the only other caller —
  `sq_read_nospin`, which deliberately obtains `start` without the spin — and asserted that
  probe SUCCEEDS. Hardening inverts it. The case now asserts the probe FAILS and that both
  out-parameters keep sentinels no committed state carries, so "nothing written through" is
  proved rather than inferred from a zero. **That assertion is the in-gate mutation proof:
  revert the predicate and it goes red on every profile, every run, every host** — which is
  branch 1 rather than a claim in a commit message.

  One recorded finding is superseded by this and is corrected rather than left standing: the
  entry spin is **no longer the SOLE guard** against consuming a mid-write state. It is one of
  two, and the two are not redundant — the retry predicate makes a no-spin reader FAIL, while
  the spin makes the composed reader WAIT and then succeed. Assertion 4 of that case is what
  keeps the second half honest.

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
