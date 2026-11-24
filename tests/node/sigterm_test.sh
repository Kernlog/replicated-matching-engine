#!/usr/bin/env bash
# A single-member node elects itself, then exits 0 after draining on SIGTERM.
set -euo pipefail
bin=$1
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
port=$(( 20000 + RANDOM % 20000 ))
"$bin" --id 1 --data-dir "$dir/data" --member "127.0.0.1:$port" --listen 127.0.0.1 --tick-ms 5 \
  > "$dir/out.log" 2>&1 &
pid=$!
for _ in $(seq 1 100); do
  grep -q "role 3" "$dir/out.log" && break
  sleep 0.05
done
kill -TERM "$pid"
wait "$pid"
grep -q "stopped cleanly" "$dir/out.log"
test -f "$dir/data/hard_state"
