#!/bin/bash

echo "=== Multi-Path SDN Controller Test Suite ==="
echo ""

cd /home/butler/Desktop/ns3-network/ns-3-dev

echo "1. Testing Single-Path Mode (baseline)..."
./ns3 run "scratch/man_of13_qkd --enableMultiPath=false --nCore=4 --ring=true --qkdStart=2.0 --qkdDur=1.0" 2>&1 | tail -n 5

echo ""
echo "2. Testing Multi-Path Mode (3 paths)..."
./ns3 run "scratch/man_of13_qkd --enableMultiPath=true --maxPaths=3 --nCore=4 --ring=true --qkdStart=2.0 --qkdDur=1.0" 2>&1 | tail -n 5

echo ""
echo "3. Testing Multi-Path with Line Topology..."
./ns3 run "scratch/man_of13_qkd --enableMultiPath=true --maxPaths=2 --nCore=4 --ring=false --qkdStart=2.0 --qkdDur=1.0" 2>&1 | tail -n 5

echo ""
echo "4. Testing Multi-Path with Link Failures..."
timeout 40 ./ns3 run "scratch/man_of13_qkd --enableMultiPath=true --maxPaths=3 --nCore=4 --ring=true --enableLinkFailures=true --qkdStart=2.0 --qkdDur=1.0" 2>&1 | tail -n 10

echo ""
echo "=== Test Suite Complete ==="
