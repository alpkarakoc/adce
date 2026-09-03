#!/usr/bin/env bash
# ADCE verification gate. Three profiles; all must pass before work is considered done.
# Runs natively on the host architecture. ARM64 is the stricter test bed for the seqlock:
# x86_64's TSO hides missing barriers that a weakly-ordered target exposes.
set -euo pipefail

cd "${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

CC="${CC:-cc}"
INC="-Iinclude"
STRICT_FLAGS=(-std=c11 -O2 -Wall -Wextra -Werror -Wconversion -Wshadow -Wcast-align
              -Wstrict-prototypes -Wpointer-arith -Wvla -pedantic)

# One list, used by every profile, so a new profile cannot be added with a
# short link line.
#
# -lm looks redundant on the development host and is not. macOS carries libm
# inside libSystem, so sqrt() links there with or without the flag; on Linux
# with GCC it does NOT -- under -O2 an errno fallback call to sqrt survives
# even for __builtin_sqrt on a provably non-negative argument, and the link
# fails with an undefined reference. Verified on linux/arm64 and linux/amd64
# under glibc 2.41. Dropping this because a macOS build still works breaks the
# shipping target and nothing else.
LDLIBS=(-lpthread -lm)
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

# --- optional repeat count ---------------------------------------------------
# How many times each profile's BINARY is re-run. The build is never repeated;
# rebuilding identical sources proves nothing and is the slow half.
#
# Default 1, so the per-edit loop is exactly as fast as it was. The scheduled CI
# run sets it higher.
#
# Why this exists: the phase race in test/t_adce_harness.c survived four green
# runs and surfaced only when a 2-core runner widened the window between a store
# and another thread observing it. TSan could not have caught it -- a
# phase-accounting race is not a data race -- and it appeared under the strict
# -O2 profile, because the sanitizers dilate execution and mask that class.
# Sampling the scheduler repeatedly is the only mechanism in this gate that
# finds it.
#
# It applies to ALL THREE profiles, deliberately, and not just the sanitized
# ones. Restricting it to the sanitizer profiles would have missed the very bug
# it was added for; strict -O2 is the most faithful timing regime here, so it is
# the one most worth re-running.
#
# This is sampling, not proof. A strictly stronger amplifier would be pinning
# every thread to one core, which maximises exactly the variable that
# discriminated -- but the harness requires the observer to publish inside
# ADCE_ADVICE_TIMEOUT_NS, and single-core pinning would starve it past that and
# produce failures that reflect the harness's own host assumptions rather than a
# defect. Unattributable reds are worse than a weaker hunt.
ADCE_REPEAT="${ADCE_REPEAT:-1}"

# Validated rather than trusted: a typo that left this empty or non-numeric
# would silently run the tests zero times and still report PASS, which is the
# one failure mode a verification gate must not have.
case "$ADCE_REPEAT" in
    ''|*[!0-9]*)
        echo "FAIL: ADCE_REPEAT must be a positive integer, got '$ADCE_REPEAT'" >&2
        exit 1
        ;;
esac
if [ "$ADCE_REPEAT" -lt 1 ]; then
    echo "FAIL: ADCE_REPEAT must be at least 1, got '$ADCE_REPEAT'" >&2
    exit 1
fi

# --- source discovery -------------------------------------------------------
# Every profile builds ONE binary from all of src/*.c plus all of test/t_*.c.
# A hardcoded file list is what allowed this gate to report green while compiling
# none of the new work, so the list is derived and an empty glob is a failure,
# never a silent zero-file build.
#
# The discovery block is duplicated in verify-linux-gcc.sh rather than shared:
# the two gates are required to change in separate commits, and a common file
# would couple them into one.
#
# Convention this enforces: exactly one test file defines main(). A second one is
# a duplicate-symbol link error here, which is the intended loud outcome -- a new
# test registers its cases in the existing runner table.
shopt -s nullglob
TEST_SRCS=(test/t_*.c)
LIB_SRCS=()
if [ -d src ]; then LIB_SRCS=(src/*.c); fi
shopt -u nullglob

if [ ${#TEST_SRCS[@]} -eq 0 ]; then
    echo "FAIL: test/t_*.c matched no files - there is nothing to verify" >&2
    exit 1
fi
# src/ not existing yet is legal. An src/ that exists but holds no translation
# unit is not: it means a file was deleted, misnamed, or never added.
if [ -d src ] && [ ${#LIB_SRCS[@]} -eq 0 ]; then
    echo "FAIL: src/ exists but src/*.c matched no files" >&2
    exit 1
fi

# bash 3.2 (the macOS system bash) treats "${empty[@]}" under 'set -u' as an error.
SRCS=(${LIB_SRCS[@]+"${LIB_SRCS[@]}"} "${TEST_SRCS[@]}")

# --- "the tests actually ran" guard -----------------------------------------
# The runner prints "TEST OK: <name>" for each case it executes. The expected set
# is read out of the SOURCE -- every `static int test_<name>(void)` definition --
# and not out of the binary. So a test file that compiles but is never wired into
# the runner table still contributes expectations that no output line satisfies,
# and the gate goes red. A t_*.c that defines no test at all is red as well, so
# "registers nothing" is not reachable by writing nothing either.
expected_tests() {
    sed -n 's/^static int test_\([A-Za-z0-9_]*\)(void).*/\1/p' "$1"
}

assert_all_tests_ran() {
    local log="$1" profile="$2" f name count bad=0
    for f in "${TEST_SRCS[@]}"; do
        count=0
        while IFS= read -r name; do
            count=$((count + 1))
            if ! grep -qxF "TEST OK: $name" "$log"; then
                echo "FAIL[$profile]: $f defines test_$name, but it never ran" >&2
                bad=1
            fi
        done < <(expected_tests "$f")
        if [ "$count" -eq 0 ]; then
            echo "FAIL[$profile]: $f defines no 'static int test_<name>(void)' case" >&2
            bad=1
        fi
    done
    return "$bad"
}

# Runs one profile's binary ADCE_REPEAT times, appending every run to one log.
#
# The caller supplies any sanitizer environment as a prefix to the call, which
# bash scopes to the function invocation. A failing run aborts here rather than
# at the end: the binary's non-zero exit propagates through the pipeline under
# 'pipefail', and 'set -e' stops the script on it, so a red run is never buried
# under the ones that follow it.
run_profile() {
    local bin="$1" log="$2" profile="$3" i=1

    : > "$log"
    while [ "$i" -le "$ADCE_REPEAT" ]; do
        if [ "$ADCE_REPEAT" -gt 1 ]; then
            echo "-- $profile: run $i/$ADCE_REPEAT --"
        fi
        "$bin" | tee -a "$log"
        i=$((i + 1))
    done

    # Once, over the accumulated log. Every run prints the full set, so a name
    # missing from all of them is still the failure this guard exists to catch.
    assert_all_tests_ran "$log" "$profile"
}

# LeakSanitizer has no Darwin back end. Leaks are only detected on Linux.
if [ "$(uname -s)" = "Linux" ]; then ASAN_LEAKS=1; else ASAN_LEAKS=0; fi

echo "== host: $(uname -s) $(uname -m) =="
echo "== sources: ${SRCS[*]} =="
if [ "$ADCE_REPEAT" -gt 1 ]; then
    echo "== repeat: each profile runs $ADCE_REPEAT times =="
fi

# The strict binary is built AND run here. Running it last meant it never executed
# while a later profile was red, leaving profile 1 as build-only.
echo "== 1/3 strict build + run =="
"$CC" "${STRICT_FLAGS[@]}" $INC "${SRCS[@]}" -o "$OUT/t_strict" "${LDLIBS[@]}"
run_profile "$OUT/t_strict" "$OUT/log-strict" "strict"

echo "== 2/3 asan + ubsan =="
"$CC" -std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
      -fno-sanitize-recover=all $INC "${SRCS[@]}" -o "$OUT/t_asan" "${LDLIBS[@]}"
ASAN_OPTIONS="detect_leaks=$ASAN_LEAKS" UBSAN_OPTIONS=print_stacktrace=1 \
    run_profile "$OUT/t_asan" "$OUT/log-asan" "asan+ubsan"

echo "== 3/3 tsan =="
# If this fails, the seqlock does not satisfy the C11 memory model. Make the payload
# fields _Atomic/relaxed. Do not write a suppression file.
"$CC" -std=c11 -O1 -g -fsanitize=thread -fno-omit-frame-pointer \
      $INC "${SRCS[@]}" -o "$OUT/t_tsan" "${LDLIBS[@]}"
TSAN_OPTIONS=halt_on_error=1 \
    run_profile "$OUT/t_tsan" "$OUT/log-tsan" "tsan"

echo "VERIFY: PASS"
