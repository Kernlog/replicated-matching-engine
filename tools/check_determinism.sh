#!/usr/bin/env bash
set -euo pipefail

dirs=()
for d in core engine consensus sim; do
  [[ -d $d ]] && dirs+=("$d")
done
[[ ${#dirs[@]} -eq 0 ]] && exit 0

banned='system_clock|steady_clock|high_resolution_clock|\btime\(|\brand\(|random_device|_distribution<|unordered_map|unordered_set|\bfloat\b|\bdouble\b|#include <thread>'

if grep -rnE "$banned" "${dirs[@]}"; then
  echo "nondeterministic API in a pure module" >&2
  exit 1
fi
