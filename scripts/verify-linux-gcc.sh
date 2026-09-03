#!/usr/bin/env bash
# Shipping-target verification: Linux + GCC.
#
# scripts/verify.sh is the fast per-edit gate and runs the HOST toolchain, which
# on the development machine is macOS/Clang on arm64. That combination cannot
# reach the shipping target, so every green run of it proves the code on
# arm64/Clang only. This script is the slower, release-facing check and is
# deliberately NOT wired into verify.sh.
#
# Two platforms, both build-and-run:
#   linux/arm64  native on an Apple-silicon host; catches GCC-specific
#                diagnostics cheaply, before paying for emulation.
#   linux/amd64  the actual shipping target. Emulated on an arm64 development
#                host, NATIVE in CI. This is the only place ADCE_CACHELINE == 64,
#                __builtin_ia32_pause, and the 64-byte _Static_asserts are ever
#                compiled.
#
# Plus one sanitizer profile: GCC ASan+UBSan, on the linux/amd64 leg, and only
# when this host runs amd64 NATIVELY.
#
# This gate previously excluded sanitizers outright, on the grounds that ASan and
# TSan under qemu report faults that do not exist on real hardware. That
# reasoning has not been abandoned -- it is still true for every emulated leg,
# and it is exactly why the run below is conditional rather than unconditional.
# What changed is that it stopped being true for amd64 in CI, which is real
# x86_64 hardware.
#
# Why it is worth running at all: Clang's sanitizers in verify.sh were the only
# ones this project had ever executed, and GCC and Clang do not report identical
# UBSan findings. adce_obs_clamp_record cites UBSan as the check that catches its
# ADCE_Q16_MIN negation guard, and the whole __int128 Q16 lane rests on the same
# check. One compiler's silence on that is one compiler's opinion.
#
# Plus one timing profile: the strict -O2 binary confined to two logical CPUs
# and run repeatedly, on the same linux/amd64-native condition. See the block
# above that profile for what it does and does not reproduce.
#
# The emulated case SKIPS and says so, with its reason. A profile that skips
# silently is worse than a profile that does not exist, because everyone believes
# it ran. Both conditional profiles share ONE native/emulated test --
# platform_is_native below -- because two mechanisms answering the same question
# is two things to get wrong. Their REASONS for skipping differ and are stated
# separately: the sanitizer profile skips because emulation reports faults that
# do not exist, and the pinned profile skips because under emulation the
# scheduling being sampled is qemu's rather than the target's. False and
# meaningless are not the same defect.
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

# Mirrors profile 2 of verify.sh exactly, including the absence of the strict
# warning set: that profile is about runtime behaviour, and the compile-time
# diagnostics are already covered by STRICT_FLAGS above.
#
# -fno-sanitize-recover=all makes the first UBSan finding fatal rather than a
# logged line the run walks past, which is the only way a sanitizer profile can
# fail a gate.
#
# TSan is deliberately NOT here. The hole this profile closes is UBSan coverage
# of the two guards named in the header; TSan is not part of it. CLAUDE.md makes
# arm64 the authoritative concurrency evidence precisely because its weak
# ordering is the stricter test, so a GCC TSan run on x86_64 TSO would be the
# weaker check under a second compiler -- new cost, little new information.
# Adding it is a separate decision with its own reason.
SAN_FLAGS="-std=c11 -O1 -g -fsanitize=address,undefined"
SAN_FLAGS="$SAN_FLAGS -fno-omit-frame-pointer -fno-sanitize-recover=all"

# --- the 2-CPU timing profile's knobs ---------------------------------------
# How many times the pinned binary runs. The build happens once inside the same
# container; rebuilding identical sources proves nothing and is the slow half.
#
# 5, and deliberately not a number derived from a target detection probability,
# because no such number can honestly be computed here. CLAUDE.md carries the
# full argument; the short form is that sizing a repeat count to a detection
# probability requires a point estimate of the per-execution failure rate, that
# estimate can only come from observed failures, and this suite is green. A green
# suite yields an upper bound on the rate and never a point estimate. 5 is chosen
# to make this profile a hunt rather than a gesture while staying proportionate
# to a gate that already takes over a minute, and it is a round number, which is
# said here rather than dressed up.
ADCE_PIN_REPEAT="${ADCE_PIN_REPEAT:-5}"

# Validated rather than trusted, for the reason verify.sh validates ADCE_REPEAT:
# an empty or non-numeric value would run the binary zero times and still let the
# profile report PASS, which is the one failure mode a verification gate must not
# have.
case "$ADCE_PIN_REPEAT" in
    ''|*[!0-9]*)
        echo "FAIL: ADCE_PIN_REPEAT must be a positive integer, got '$ADCE_PIN_REPEAT'" >&2
        exit 1
        ;;
esac
if [ "$ADCE_PIN_REPEAT" -lt 1 ]; then
    echo "FAIL: ADCE_PIN_REPEAT must be at least 1, got '$ADCE_PIN_REPEAT'" >&2
    exit 1
fi

# The two logical CPUs the profile is confined to. Fixed rather than
# configurable: "two" is the entire claim this profile makes, and a knob would
# invite running it with some other number and reporting the result under the
# same name.
ADCE_PIN_CPUS="0,1"

# Which legs this host can run without emulation. macOS reports arm64 and Linux
# reports aarch64 for the same hardware, so both spellings map to the same leg.
# Anything unrecognised falls through to "emulated", which skips: a profile whose
# output cannot be trusted must not run, and guessing in the permissive direction
# would produce exactly the false reports this gate used to avoid by excluding
# sanitizers altogether.
HOST_ARCH="$(uname -m)"

platform_is_native() {
    case "$1:$HOST_ARCH" in
        linux/amd64:x86_64|linux/amd64:amd64) return 0 ;;
        linux/arm64:arm64|linux/arm64:aarch64) return 0 ;;
        *) return 1 ;;
    esac
}

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

    # The sanitizer profile. amd64 only, native only, and never silent about
    # which of those two it did.
    if [ "$platform" = "linux/amd64" ]; then
        sanlog="$OUT/log-amd64-san"
        if platform_is_native "$platform"; then
            echo "== $platform gcc asan+ubsan (native $HOST_ARCH) =="
            if docker run --rm --platform "$platform" -v "$PWD":/src:ro -w /src "$IMAGE" \
                 sh -c "gcc $SAN_FLAGS -Iinclude $SRC_LIST -o /tmp/t_gcc_san $LDLIBS && \
                        ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
                        /tmp/t_gcc_san" 2>&1 | tee "$sanlog" \
               && assert_all_tests_ran "$sanlog" "$platform gcc-san"; then
                echo "== $platform gcc asan+ubsan: PASS =="
            else
                echo "== $platform gcc asan+ubsan: FAIL ==" >&2
                failed="$failed $platform/asan+ubsan"
            fi
        else
            # Loud, and on stdout with the rest of the report rather than buried
            # in stderr: this is the difference between "GCC's sanitizers passed"
            # and "GCC's sanitizers were not run here", and those are not the
            # same claim.
            echo "== $platform gcc asan+ubsan: SKIPPED =="
            echo "   this host is $HOST_ARCH, so linux/amd64 runs under qemu, and"
            echo "   ASan/UBSan under emulation report faults that do not exist on"
            echo "   real hardware. The profile runs where amd64 is native -- CI."
            echo "   GCC's sanitizers were NOT run by this invocation."
        fi

        # --- the 2-CPU timing profile ---------------------------------------
        # WHY IT EXISTS. ADCE_REPEAT=10 in the CI workflow was derived from a
        # per-execution failure rate of about 0.067, measured on 2-vCPU runners.
        # GitHub widened those runners to 4 vCPU on 2026-09-03 and the 2-core
        # configuration left the matrix, so the only timing bug this project has
        # ever found was found on hardware that no longer exists in it.
        #
        # WHAT PINNING IS. --cpuset-cpus confines THIS CONTAINER's threads to two
        # logical CPUs of a larger machine. The container's own nproc reports 2,
        # which is echoed below so the constraint is evidence in the log rather
        # than an assertion in a comment.
        #
        # WHAT PINNING IS NOT, and this is the part that must not be overstated:
        # it is not a 2-vCPU machine. On a genuinely 2-vCPU runner the kernel,
        # the runner agent and every other process compete for the same two CPUs.
        # Here the rest of the system still has CPUs 2..n-1, and is not excluded
        # from 0 and 1 either -- cpuset confines us to them, it does not reserve
        # them for us. So this reproduces contention AMONG THIS BINARY's threads
        # on two CPUs; it does not reproduce system-wide CPU scarcity.
        #
        # It therefore does not restore the lost coverage. It produces DIFFERENT
        # coverage, and which of the two the phase race needed is UNKNOWN. There
        # is exactly one observation of that race, run 33748342781, with no
        # instrumentation of what the scheduler did, and the bug is fixed so no
        # further samples can be drawn. One sample cannot separate "needed
        # system-wide scarcity" from "needed intra-process contention on two
        # CPUs", and this comment will not pretend otherwise.
        #
        # STRICT -O2, not a sanitized build, and that is not an oversight:
        # CLAUDE.md records that the phase race appeared under strict and that
        # ASan and TSan dilate execution enough to mask that entire class.
        pinlog="$OUT/log-amd64-pin"
        if platform_is_native "$platform"; then
            echo "== $platform strict on cpus $ADCE_PIN_CPUS, x$ADCE_PIN_REPEAT (native $HOST_ARCH) =="
            if docker run --rm --platform "$platform" --cpuset-cpus="$ADCE_PIN_CPUS" \
                 -v "$PWD":/src:ro -w /src "$IMAGE" \
                 sh -c "echo \"visible cpus: \$(nproc)\" && \
                        gcc $STRICT_FLAGS -Iinclude $SRC_LIST -o /tmp/t_pin $LDLIBS && \
                        i=1 && while [ \$i -le $ADCE_PIN_REPEAT ]; do \
                          echo \"-- pinned run \$i/$ADCE_PIN_REPEAT --\"; \
                          /tmp/t_pin || exit 1; \
                          i=\$((i + 1)); \
                        done" 2>&1 | tee "$pinlog" \
               && assert_all_tests_ran "$pinlog" "$platform pinned"; then
                echo "== $platform strict pinned: PASS =="
            else
                echo "== $platform strict pinned: FAIL ==" >&2
                failed="$failed $platform/pinned"
            fi
        else
            # Same condition as the sanitizer profile, different reason. That one
            # skips because emulation reports faults that are not real; this one
            # skips because under emulation the scheduling being sampled is
            # qemu's, so a green result would say nothing about the target and a
            # red one could not be attributed. Meaningless, not false.
            echo "== $platform strict pinned to 2 CPUs: SKIPPED =="
            echo "   this host is $HOST_ARCH, so linux/amd64 runs under qemu, and"
            echo "   a timing profile under emulation samples the emulator's"
            echo "   scheduler rather than the target's. The profile runs where"
            echo "   amd64 is native -- CI."
            echo "   The 2-CPU regime was NOT exercised by this invocation."
        fi
    fi
    echo
done

if [ -n "$failed" ]; then
    echo "VERIFY-LINUX-GCC: FAIL ($(echo "$failed" | sed 's/^ //'))" >&2
    exit 1
fi

echo "VERIFY-LINUX-GCC: PASS"
