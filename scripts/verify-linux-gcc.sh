#!/usr/bin/env bash
# Shipping-target verification: Linux + GCC.
#
# scripts/verify.sh is the fast per-edit gate and runs the HOST toolchain, which
# on the development machine is macOS/Clang on arm64. That combination cannot
# reach the shipping target, so every green run of it proves the code on
# arm64/Clang only. This script is the slower, release-facing check and is
# deliberately NOT wired into verify.sh.
#
# Two profiles, both build-and-run:
#   linux/arm64  native on an Apple-silicon host; catches GCC-specific
#                diagnostics cheaply, before paying for emulation.
#   linux/amd64  emulated, and the actual shipping target. This is the only
#                place ADCE_CACHELINE == 64, __builtin_ia32_pause, and the
#                64-byte _Static_asserts are ever compiled.
#
# No sanitizers here on purpose: ASan/TSan under qemu emulation report races and
# faults that do not exist on real hardware. They stay in verify.sh, where they
# run natively and their output can be trusted.
set -euo pipefail

cd "${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

IMAGE="${ADCE_GCC_IMAGE:-gcc:14}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

# The same strict set as profile 1 of verify.sh, -pedantic and -Werror included.
# Kept in sync by hand rather than shared through a common file: the two gates
# are required to change in separate commits, and a shared file would couple
# them into one.
STRICT_FLAGS="-std=c11 -O2 -Wall -Wextra -Werror -Wconversion -Wshadow"
STRICT_FLAGS="$STRICT_FLAGS -Wcast-align -Wstrict-prototypes -Wpointer-arith -Wvla -pedantic"

# -lm is load-bearing HERE in a way it is not on the development host. macOS
# carries libm inside libSystem, so verify.sh links sqrt() with or without the
# flag and a host-only check would never notice its absence; GCC on Linux keeps
# an errno fallback call to sqrt under -O2 -- even for __builtin_sqrt on a
# provably non-negative argument -- and the link fails with an undefined
# reference. Confirmed on both platforms below under glibc 2.41. This gate is
# the only place that failure is observable.
LDLIBS="-lpthread -lm"

# --- source discovery -------------------------------------------------------
# Each platform builds ONE binary from all of src/*.c plus all of test/t_*.c.
# A hardcoded file list is what allowed this gate to report green while compiling
# none of the new work, so the list is derived and an empty glob is a failure,
# never a silent zero-file build. Duplicated from verify.sh by the same rule that
# duplicates the flags above.
#
# Convention this enforces: exactly one test file defines main(). A second one is
# a duplicate-symbol link error inside the container, which is the intended loud
# outcome -- a new test registers its cases in the existing runner table.
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
# Space-joined for the container's `sh -c`. Safe because these are repo-relative
# paths under src/ and test/, which carry no spaces; the mount makes $PWD the
# container's working directory, so the relative paths resolve unchanged.
SRC_LIST="${SRCS[*]}"

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

# An unavailable Docker means the shipping target was NOT verified. Say so and
# fail; reporting success here would be the single most misleading thing this
# script could do.
if ! command -v docker >/dev/null 2>&1; then
    echo "FAIL: docker is not installed - the shipping target was NOT built" >&2
    exit 1
fi

if ! docker info >/dev/null 2>&1; then
    echo "FAIL: the Docker daemon is not reachable - the shipping target was NOT built" >&2
    echo "      start Docker and re-run; do not treat this as a pass" >&2
    exit 1
fi

echo "== sources: $SRC_LIST =="

# A string, not an array: macOS ships bash 3.2, where expanding an empty array
# under 'set -u' is itself an error.
failed=""

for platform in linux/arm64 linux/amd64; do
    echo "== $platform ($IMAGE) =="
    log="$OUT/log-$(printf '%s' "$platform" | tr / -)"
    # The run is teed rather than left on the terminal so the ran-tests guard has
    # the output to check; 'pipefail' keeps the container's exit status decisive.
    if docker run --rm --platform "$platform" -v "$PWD":/src:ro -w /src "$IMAGE" \
         sh -c "gcc --version | head -1 && uname -m && \
                gcc $STRICT_FLAGS -Iinclude $SRC_LIST -o /tmp/t_gcc $LDLIBS && \
                /tmp/t_gcc" 2>&1 | tee "$log" \
       && assert_all_tests_ran "$log" "$platform"; then
        echo "== $platform: PASS =="
    else
        echo "== $platform: FAIL ==" >&2
        failed="$failed $platform"
    fi
    echo
done

if [ -n "$failed" ]; then
    echo "VERIFY-LINUX-GCC: FAIL ($(echo "$failed" | sed 's/^ //'))" >&2
    exit 1
fi

echo "VERIFY-LINUX-GCC: PASS"
