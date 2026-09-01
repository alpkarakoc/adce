#!/usr/bin/env bash
set -uo pipefail

input="$(cat)"
if [ "$(printf '%s' "$input" | jq -r '.stop_hook_active // false')" = "true" ]; then
  exit 0
fi

cd "${CLAUDE_PROJECT_DIR:-.}" || exit 0

if ! out="$(./scripts/verify.sh 2>&1)"; then
  printf 'verify.sh failed. Fix before finishing:\n%s\n' "$(printf '%s' "$out" | tail -n 60)" >&2
  exit 2
fi
exit 0