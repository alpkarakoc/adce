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
SRC="test/t_adce_platform.c"

# The same strict set as profile 1 of verify.sh, -pedantic and -Werror included.
# Kept in sync by hand rather than shared through a common file: the two gates
# are required to change in separate commits, and a shared file would couple
# them into one.
STRICT_FLAGS="-std=c11 -O2 -Wall -Wextra -Werror -Wconversion -Wshadow"
STRICT_FLAGS="$STRICT_FLAGS -Wcast-align -Wstrict-prototypes -Wpointer-arith -Wvla -pedantic"

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

# A string, not an array: macOS ships bash 3.2, where expanding an empty array
# under 'set -u' is itself an error.
failed=""

for platform in linux/arm64 linux/amd64; do
    echo "== $platform ($IMAGE) =="
    if docker run --rm --platform "$platform" -v "$PWD":/src:ro -w /src "$IMAGE" \
         sh -c "gcc --version | head -1 && uname -m && \
                gcc $STRICT_FLAGS -Iinclude $SRC -o /tmp/t_gcc -lpthread && \
                /tmp/t_gcc"; then
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
