#!/usr/bin/env bash
# Runs a 3-node cluster as local processes and drives it with the open-loop load generator.
# usage: bench/cluster.sh <rate> [seconds] [hgrm-out] ; NODE_FLAGS adds flags to every node,
# CPUS="1,9,2 3,11,4 5,13,6" pins node i's core, network and storage threads.
set -euo pipefail
cd "$(dirname "$0")/.."
rate=$1 seconds=${2:-10} hgrm=${3:-}
bin=${BIN:-build/release}
dir=$(mktemp -d)
pids=()
trap 'kill "${pids[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$dir"' EXIT
read -r -a cpus <<< "${CPUS:-}"
base=$(( 21000 + RANDOM % 20000 ))
members=()
for i in 1 2 3; do members+=(--member "127.0.0.1:$((base + i))"); done
for i in 1 2 3; do
  pin=()
  [ ${#cpus[@]} -eq 3 ] && pin=(--cpus ${cpus[$((i - 1))]//,/ })
  "$bin/node/rme_node" --id "$i" --data-dir "$dir/$i" --listen 127.0.0.1 \
    --client-port $((base + 10 + i)) "${members[@]}" "${pin[@]}" ${NODE_FLAGS:-} \
    > "$dir/node$i.log" 2>&1 &
  pids+=($!)
done
sleep 1.5
"$bin/tools/loadgen" --member 127.0.0.1:$((base + 11)) --member 127.0.0.1:$((base + 12)) \
  --member 127.0.0.1:$((base + 13)) --rate "$rate" --seconds "$seconds" \
  --connections "${CONNECTIONS:-4}" ${hgrm:+--hgrm "$hgrm"}
