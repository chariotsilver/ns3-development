#!/bin/bash

echo "=== Validating Commit-on-Close with Deduplication ==="
echo "Testing that key bits are committed immediately on window close"
echo "and that ACKs only update RTT telemetry without double-counting"
echo

cd ns-3-dev

# Run a QKD test to validate the commit-on-close behavior
echo "Running QKD simulation with commit-on-close deduplication..."
./ns3 run "scratch/man_of13_qkd --verbosity=2 --qkd-test=load --pulse-rate=10000" 2>&1 | tee ../commit_test.log

echo
echo "=== Checking for immediate commits on window close ==="
grep -i "closewindow\|commit.*immediately\|optimistic.*commit" ../commit_test.log | head -10

echo
echo "=== Checking for ACK telemetry updates ==="
grep -i "sift.*ack.*received\|updating.*rtt\|rtt.*telemetry" ../commit_test.log | head -5

echo
echo "=== Checking for key buffer progression ==="
echo "Key buffer should show immediate progress even with delayed ACKs:"
grep -i "key buffer\|secret bits.*window\|lastbits" ../commit_test.log | tail -10

echo
echo "=== Checking for deduplication behavior ==="
echo "Looking for idempotent behavior (should not see double commits for same wid):"
grep -i "idempotent\|already.*committed\|dedupe" ../commit_test.log

echo
echo "=== Summary: Key Buffer Growth Analysis ==="
echo "Expected: Non-zero key buffer even if ACKs are delayed"
echo "Actual key buffer values:"
grep -E "buffer.*[0-9]+|bits.*[0-9]+" ../commit_test.log | tail -5

echo
echo "Validation complete. Check commit_test.log for detailed output."
