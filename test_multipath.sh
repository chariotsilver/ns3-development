#!/bin/bash

echo "=== Enhanced SDN Controller Test Suite ==="
echo ""

cd /home/butler/Desktop/ns3-network/ns-3-dev

echo "1. Testing Single-Path Mode + Basic QoS (baseline)..."
./ns3 run "scratch/man_of13_qkd --enableMultiPath=false --enableEnhancedQoS=false --nCore=4 --ring=true --qkdStart=2.0 --qkdDur=1.0" 2>&1 | tail -n 5

echo ""
echo "2. Testing Multi-Path Mode + Enhanced QoS..."
./ns3 run "scratch/man_of13_qkd --enableMultiPath=true --maxPaths=3 --enableEnhancedQoS=true --nCore=4 --ring=true --qkdStart=2.0 --qkdDur=1.0" 2>&1 | tail -n 5

echo ""
echo "3. Testing Enhanced QoS Only (no multi-path)..."
./ns3 run "scratch/man_of13_qkd --enableMultiPath=false --enableEnhancedQoS=true --nCore=4 --ring=true --qkdStart=2.0 --qkdDur=1.0" 2>&1 | tail -n 5

echo ""
echo "4. Testing All Features with Link Failures..."
timeout 40 ./ns3 run "scratch/man_of13_qkd --enableMultiPath=true --maxPaths=3 --enableEnhancedQoS=true --nCore=4 --ring=true --enableLinkFailures=true --qkdStart=2.0 --qkdDur=1.0" 2>&1 | tail -n 10

echo ""
echo "5. Testing QoS with Different Traffic Classes..."
./ns3 run "scratch/man_of13_qkd --enableEnhancedQoS=true --qosMark=EF --nCore=6 --ring=true --qkdStart=2.0 --qkdDur=1.0" 2>&1 | tail -n 5

echo ""
echo "=== Test Suite Complete ==="
