#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if ! command -v clang-format >/dev/null; then
    echo "clang-format is required" >&2
    exit 1
fi
case "${1:---check}" in
    --check) mode=(--dry-run --Werror) ;;
    --fix) mode=(-i) ;;
    *) echo "Usage: $0 [--check|--fix]" >&2; exit 2 ;;
esac
find "$root/include" "$root/apps" "$root/tests" -type f \( -name '*.hpp' -o -name '*.cpp' \) -print0 | xargs -0 -r clang-format "${mode[@]}"
