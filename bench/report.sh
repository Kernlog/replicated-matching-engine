#!/usr/bin/env bash
# Regenerates every number in docs/benchmarks.md into an output directory.
# usage: bench/report.sh <out-dir>
set -euo pipefail
cd "$(dirname "$0")/.."
out=$1
mkdir -p "$out"
taskset -c 2 build/bench/bench/engine_bench --benchmark_repetitions=3 \
  --benchmark_report_aggregates_only=true > "$out/engine_bench.txt"
taskset -c 2,4 build/bench/bench/queue_bench --benchmark_repetitions=3 \
  --benchmark_report_aggregates_only=true > "$out/queue_bench.txt"
taskset -c 2,4 build/bench/bench/io_bench > "$out/io_bench.txt"
pin="2,8,3 4,10,5 6,12,7"
for rate in 10000 50000 100000; do
  LOADGEN_CPUS=14,15 CPUS="$pin" DATA_ROOT=/dev/shm bench/cluster.sh "$rate" 20 "$out/memory-$rate.hgrm" \
    | tail -1 >> "$out/latency.txt" || true
done
for rate in 1000 5000 20000; do
  LOADGEN_CPUS=14,15 CPUS="$pin" bench/cluster.sh "$rate" 20 "$out/disk-$rate.hgrm" \
    | tail -1 | sed 's/^/disk /' >> "$out/latency.txt" || true
done
python3 tools/plot_hgrm.py "$out/latency-memory.svg" \
  "10k/s=$out/memory-10000.hgrm" "50k/s=$out/memory-50000.hgrm" "100k/s=$out/memory-100000.hgrm"
python3 tools/plot_hgrm.py "$out/latency-disk.svg" \
  "1k/s=$out/disk-1000.hgrm" "5k/s=$out/disk-5000.hgrm" "20k/s=$out/disk-20000.hgrm"
