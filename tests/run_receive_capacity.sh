#!/usr/bin/env bash
set -u
binary="${1:-./build/receive_capacity_tests}"
status=0
for scenario in sanity shared_committed shared_inflight independent independent_same direct_committed direct_reserved interleave resume direct_mix independent_direct tail_commit_retry; do
  "$binary" "$scenario" || status=1
done
exit "$status"
