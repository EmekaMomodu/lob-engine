#!/usr/bin/env bash
# Interleaved A/B: run each binary alternately, `rounds` times, 5 throughput runs each.
set -euo pipefail
dir="$1"; shift
rounds="$1"; shift
for r in $(seq 1 "$rounds"); do
  for bin in "$@"; do
    "$dir/$bin" --mode throughput --engine map,array --workload realistic,thin \
      --ops 2000000 --warmup 200000 --runs 5 --json "$dir/${bin}_r${r}.json" >/dev/null
  done
done
