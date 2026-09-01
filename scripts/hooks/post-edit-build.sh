#!/usr/bin/env bash
# Her .c/.h yazımından sonra hızlı katı derleme. Ucuz; ihlali modelin önüne
# oturum sonunda değil, bir sonraki turda koyar.
set -uo pipefail

input="$(cat)"
path="$(printf '%s' "$input" | jq -r '.tool_input.file_path // empty')"
case "$path" in
  *.c|*.h) ;;
  *) exit 0 ;;
esac

cd "${CLAUDE_PROJECT_DIR:-.}" || exit 0

if ! out="$(cc -std=c11 -O2 -Wall -Wextra -Werror -Wconversion -pedantic \
             -fsyntax-only -Iinclude test/t_adce_platform.c 2>&1)"; then
  printf 'Strict C11 build broken by %s:\n%s\n' "$path" "$out" >&2
  exit 2
fi
exit 0