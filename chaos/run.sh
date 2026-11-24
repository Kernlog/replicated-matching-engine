#!/usr/bin/env bash
# One chaos run: fresh cluster, workload and nemesis side by side, then a clean stop so every
# node's WAL is final for the checker.
set -euo pipefail
cd "$(dirname "$0")/.."
faults=${FAULTS:-kill,partition,latency,pause}
seconds=${SECONDS_PER_RUN:-60}
seed=${SEED:-1}
out=${OUT:-chaos/runs/$faults-$seed}
client=${CLIENT:-build/release/tools/chaos_client}
mkdir -p "$out"
docker compose down -v --remove-orphans >/dev/null 2>&1 || true
docker compose up -d --wait >/dev/null
"$client" --member 127.0.0.1:7001 --member 127.0.0.1:7002 --member 127.0.0.1:7003 \
  --workers "${WORKERS:-8}" --seconds "$seconds" --out "$out/history.tsv" &
workload=$!
python3 chaos/nemesis.py --faults "$faults" --seconds "$((seconds - 10))" --seed "$seed" --out "$out"
wait "$workload"
docker compose stop >/dev/null
for node in node1 node2 node3; do
  docker compose cp "$node:/data" "$out/$node" >/dev/null
done
echo "$out: $(wc -l < "$out/history.tsv") operations, $(grep -c . "$out/nemesis.tsv") faults"
