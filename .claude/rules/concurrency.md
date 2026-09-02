---
paths:
  - "include/**/*.h"
  - "src/**/*.c"
  - "test/**/*.c"
---

# ADCE concurrency rules

- A data race is undefined behaviour in the C11 memory model. A seqlock is NOT an
  exception. Every payload field read concurrently with a write must be `_Atomic` and
  accessed with `memory_order_relaxed`.
- Seqlock writer: increment seq (release) -> write payload (relaxed) -> increment seq
  (release).
  Seqlock reader: read seq (acquire) -> read payload (relaxed) ->
  `atomic_thread_fence(memory_order_acquire)` -> re-read seq and compare.
- A correctly written seqlock is clean under ThreadSanitizer. A TSan report means the
  protocol is incomplete, not that TSan is wrong. Writing a suppression file is forbidden.
- No mutex, spinlock, malloc, or free on the publication path.
- Time source is `CLOCK_MONOTONIC_RAW` only. `CLOCK_REALTIME` can step backwards.
- The publication object occupies exactly one `ADCE_CACHELINE`. `_Alignas` goes on the
  first member (C11 §6.7.5p2 forbids it on a typedef); size is pinned by `_Static_assert`.
- Q16.16 arithmetic widens intermediate products to `__int128`, a locked dependency
  guarded by the `#error` at the top of the header. `ADCE_Q16_MAX` is `INT64_MAX`, so
  `int64_t` is too narrow. Overflow and saturation are tested at the boundary.
- New behaviour arrives with its test in the same commit. Nothing is finished until
  `./scripts/verify.sh` is green in all three profiles.
