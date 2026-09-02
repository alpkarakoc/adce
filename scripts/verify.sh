#!/usr/bin/env bash
# ADCE verification gate. Three profiles; all must pass before work is considered done.
# Runs natively on the host architecture. ARM64 is the stricter test bed for the seqlock:
# x86_64's TSO hides missing barriers that a weakly-ordered target exposes.
set -euo pipefail

cd "${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

CC="${CC:-cc}"
SRC="test/t_adce_platform.c"
INC="-Iinclude"
STRICT_FLAGS=(-std=c11 -O2 -Wall -Wextra -Werror -Wconversion -Wshadow -Wcast-align
              -Wstrict-prototypes -Wpointer-arith -Wvla -pedantic)
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

# LeakSanitizer has no Darwin back end. Leaks are only detected on Linux.
if [ "$(uname -s)" = "Linux" ]; then ASAN_LEAKS=1; else ASAN_LEAKS=0; fi

echo "== host: $(uname -s) $(uname -m) =="

# The strict binary is built AND run here. Running it last meant it never executed
# while a later profile was red, leaving profile 1 as build-only.
echo "== 1/3 strict build + run =="
"$CC" "${STRICT_FLAGS[@]}" $INC "$SRC" -o "$OUT/t_strict" -lpthread
"$OUT/t_strict"

echo "== 2/3 asan + ubsan =="
"$CC" -std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
      -fno-sanitize-recover=all $INC "$SRC" -o "$OUT/t_asan" -lpthread
ASAN_OPTIONS="detect_leaks=$ASAN_LEAKS" UBSAN_OPTIONS=print_stacktrace=1 "$OUT/t_asan"

echo "== 3/3 tsan =="
# If this fails, the seqlock does not satisfy the C11 memory model. Make the payload
# fields _Atomic/relaxed. Do not write a suppression file.
"$CC" -std=c11 -O1 -g -fsanitize=thread -fno-omit-frame-pointer \
      $INC "$SRC" -o "$OUT/t_tsan" -lpthread
TSAN_OPTIONS=halt_on_error=1 "$OUT/t_tsan"

echo "VERIFY: PASS"
