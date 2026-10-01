#!/usr/bin/env bash
set -u
binary="${1:-./build/receive_capacity_tests}"
status=0
for scenario in sanity shared_committed shared_inflight independent independent_same direct_committed direct_reserved cross_vc_interleave recover_after_free direct_reserved_interleave independent_direct_other_pool tail_commit_retry return_command_special; do
  "$binary" "$scenario" || status=1
done
exit "$status"
