#!/usr/bin/env bash
# ADCE doğrulama kapısı. Üç ayrı derleme profili; hepsi geçmeden iş bitmiş sayılmaz.
set -euo pipefail

cd "${CLAUDE_PROJECT_DIR:-$(git rev-parse --show-toplevel)}"

CC="${CC:-cc}"
STRICT="-std=c11 -O2 -Wall -Wextra -Werror -Wconversion -Wshadow -Wcast-align
        -Wstrict-prototypes -Wpointer-arith -Wvla -pedantic"
SRC="test/t_adce_platform.c"
INC="-Iinclude"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

echo "== 1/3 strict build =="
$CC $STRICT $INC "$SRC" -o "$OUT/t_strict" -lpthread

echo "== 2/3 asan + ubsan =="
$CC -std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -fno-sanitize-recover=all $INC "$SRC" -o "$OUT/t_asan" -lpthread
# LeakSanitizer has no Darwin back end; asking for it aborts the binary before
# a single test runs. It stays enforced on the Linux shipping target.
ASAN_LEAKS=0
if [ "$(uname -s)" = "Linux" ]; then ASAN_LEAKS=1; fi
ASAN_OPTIONS="detect_leaks=$ASAN_LEAKS" UBSAN_OPTIONS=print_stacktrace=1 "$OUT/t_asan"

echo "== 3/3 tsan =="
# Seqlock payload'ı relaxed atomic ise bu profil temiz geçer. Geçmiyorsa kod C11
# bellek modeline uymuyor demektir — suppression yazma, payload'ı _Atomic yap.
$CC -std=c11 -O1 -g -fsanitize=thread -fno-omit-frame-pointer \
    $INC "$SRC" -o "$OUT/t_tsan" -lpthread
TSAN_OPTIONS=halt_on_error=1 "$OUT/t_tsan"

echo "== 1/1 strict run =="
"$OUT/t_strict"

echo "VERIFY: PASS"