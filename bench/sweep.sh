#!/usr/bin/env bash
# Throughput and latency as the number of commands packed into one log entry grows.
# usage: bench/sweep.sh [rate] [seconds]
set -euo pipefail
cd "$(dirname "$0")/.."
rate=${1:-100000} seconds=${2:-8}
printf '| commands per entry | acked/s | p50 us | p99 us | p99.9 us |\n|---|---|---|---|---|\n'
for commands in 1 8 64 512 1365; do
  line=$(NODE_FLAGS="--max-entry-bytes $((commands * 48)) ${NODE_FLAGS:-}" \
    bench/cluster.sh "$rate" "$seconds" 2>&1 | tail -1 || true)
  acked=$(sed -n 's/.*acked=\([0-9]*\).*/\1/p' <<<"$line")
  get() { sed -n "s/.*$1=\([0-9.]*\).*/\1/p" <<<"$line"; }
  printf '| %s | %s | %s | %s | %s |\n' "$commands" "$(( ${acked:-0} / (seconds + 2) ))" \
    "$(get p50_us)" "$(get p99_us)" "$(get 'p99.9_us')"
done
