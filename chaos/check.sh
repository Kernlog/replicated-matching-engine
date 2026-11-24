#!/usr/bin/env bash
# Checks one chaos run directory: the WAL audit, then Porcupine over the client history.
set -euo pipefail
cd "$(dirname "$0")/.."
run=$1
audit=${AUDIT:-build/release/tools/audit}
"$audit" --history "$run/history.tsv" --data "$run/node1" --data "$run/node2" --data "$run/node3"
(cd chaos/porcupine && go run . "../../$run/history.tsv")
