#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

mode=write
case "${1:-}" in
  "") ;;
  --check) mode=check; shift ;;
  --list) mode=list; shift ;;
  -h|--help)
    printf 'Usage: bash scripts/format.sh [--check|--list]\n'
    printf 'Format tracked Starfish C++ under runtime/include/, runtime/src/, and tests/.\n'
    printf 'Upstream apps, third_party, and the Microsoft Zipfian header are excluded.\n'
    printf 'Requires clang-format 20; override its path with CLANG_FORMAT.\n'
    exit 0 ;;
  *) printf 'format: unknown option: %s\n' "$1" >&2; exit 2 ;;
esac
[[ $# -eq 0 ]] || { printf 'format: unexpected arguments\n' >&2; exit 2; }

files=()
while IFS= read -r -d '' file; do
  case "$file" in
    runtime/include/lib/zipfian.hpp) continue ;;
    *.h|*.hpp|*.cc|*.cpp|*.ipp) files+=("$file") ;;
  esac
done < <(git ls-files -z -- runtime/include runtime/src tests)
(("${#files[@]}" > 0)) || { printf 'format: no owned C++ files found\n' >&2; exit 2; }

if [[ "$mode" = list ]]; then
  printf '%s\n' "${files[@]}"
  exit 0
fi

formatter=${CLANG_FORMAT:-clang-format-20}
command -v "$formatter" >/dev/null ||
  { printf 'format: install clang-format 20 or set CLANG_FORMAT\n' >&2; exit 2; }
version=$("$formatter" --version)
[[ "$version" =~ version[[:space:]]20\. ]] ||
  { printf 'format: clang-format 20 required; found %s\n' "$version" >&2; exit 2; }
printf '%s\n' "$version"
printf 'Starfish-owned C++ files: %s\n' "${#files[@]}"

args=(--style="file:$ROOT/.clang-format" --fail-on-incomplete-format)
if [[ "$mode" = check ]]; then
  args+=(--dry-run --Werror --ferror-limit=1)
else
  args+=(-i)
fi
"$formatter" "${args[@]}" "${files[@]}"
printf 'format: %s PASS\n' "$mode"
