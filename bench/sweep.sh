#!/usr/bin/env bash
# For each count of commands packed into one log entry, the highest offered rate the cluster
# applies in full, and the latency at that rate.
# usage: bench/sweep.sh [seconds] ; RATES overrides the rates tried, in increasing order.
set -euo pipefail
cd "$(dirname "$0")/.."
seconds=${1:-8}
read -r -a rates <<< "${RATES:-25000 50000 100000 150000 200000 300000}"
printf '| commands per entry | sustained rate/s | p50 us | p99 us | p99.9 us |\n|---|---|---|---|---|\n'
for commands in ${COMMANDS:-1 8 64 512 1365}; do
  best="" line=""
  for rate in "${rates[@]}"; do
    if run=$(NODE_FLAGS="--max-entry-bytes $((commands * 48)) ${NODE_FLAGS:-}" \
        bench/cluster.sh "$rate" "$seconds" 2>&1 | tail -1); then
      best=$rate line=$run
    else
      break
    fi
  done
  get() { sed -n "s/.*$1=\([0-9.]*\).*/\1/p" <<<"$line"; }
  printf '| %s | %s | %s | %s | %s |\n' "$commands" "${best:-<${rates[0]}}" "$(get p50_us)" \
    "$(get p99_us)" "$(get 'p99.9_us')"
done
