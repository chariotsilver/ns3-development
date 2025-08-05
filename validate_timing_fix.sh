#!/bin/bash

echo "=== Validating QKD Timing Fix ==="
echo "Testing that responder starts before sifting app to prevent ACK starvation"
echo

cd ns-3-dev

# Run a short QKD test to validate the timing fix
echo "Running QKD simulation with timing fix..."
./ns3 run "scratch/man_of13_qkd --verbosity=2 --qkd-test=load --pulse-rate=10000" 2>&1 | tee ../timing_test.log

# Check for key buffer accumulation (should be > 0 with proper ACK timing)
echo
echo "=== Checking for key buffer accumulation ==="
grep -i "key buffer\|secret bits\|window.*committed" ../timing_test.log | tail -10

echo
echo "=== Checking for ACK reception ==="
grep -i "ack\|sift.*response" ../timing_test.log | head -5

echo
echo "=== Checking timing sequence ==="
grep -E "(responder.*start|sift.*start|simulation.*stop)" ../timing_test.log

echo
echo "Validation complete. Check timing_test.log for detailed output."
