# ADCE — Anomaly Detection & Stochastic Containment Engine

C11 plus one named compiler extension (`__int128`). No external dependencies. Target: lock-free, allocation-free, fail-closed
publication path.

## Commands

- `./scripts/verify.sh` — the gate: strict build + ASan/UBSan + TSan + test run.
  A turn is not finished until all three profiles are green.
- Quick syntax check:
  `cc -std=c11 -O2 -Wall -Wextra -Werror -Iinclude test/t_adce_platform.c -lpthread`

## Layout

- `include/adce_platform.h` — single header, built on C11 `stdatomic.h` acquire/release.
  No mutexes, no spinlocks, no allocation.
- `test/t_adce_platform.c` — `t_*` unit tests: Q16.16 arithmetic (including overflow),
  token-bucket clamping, RNG sanity, time monotonicity, single-threaded seqlock, and a
  two-thread stress test.

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
- Open risk, not yet verified: GCC emits a pedwarn for `__int128` under `-pedantic`, which
  `-Werror` turns into a build failure. Clang does not, which is why profile 1 passes on
  the macOS dev host. The shipping target has never been built. A Linux/GCC profile is
  required before any release claim.
- `adce_q16_div` with a zero divisor saturates toward the numerator's sign: negative
  numerator yields `ADCE_Q16_MIN`, zero or positive yields `ADCE_Q16_MAX`. A collapsed
  divisor therefore reads as maximal pressure downstream, never as zero. `0 / 0` is
  `ADCE_Q16_MAX` by this rule. This is a fail-closed contract; changing it is an API
  decision.
- Open defect, not yet scheduled: `adce_q16_to_int` floors negative values (arithmetic
  right shift) while `adce_q16_div` truncates toward zero (C integer division). The two
  disagree on non-integral negatives. Resolving it is an API decision — pick one rounding
  mode, apply it to both, and test the boundary. Do not change either in isolation.
