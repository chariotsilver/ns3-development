#!/usr/bin/env bash
# Usage: bash validate_man_of13_qkd.sh
set -u

STAMP=$(date +%Y%m%d-%H%M%S)
OUTDIR="man_of13_qkd_results_${STAMP}"
mkdir -p "$OUTDIR"

# Minimal CSV for the CSV test
cat > "$OUTDIR/man_topo.csv" <<'EOF'
h0,s0,spur,1Gbps,0.2ms
h1,s1,spur,1Gbps,0.2ms
s0,s1,core,10Gbps,0.5ms
EOF

run() {
  local name="$1"; shift
  echo "=== RUN $name ===" | tee -a "$OUTDIR/SUMMARY.txt"
  # Enable controller logs (incl. teardown dpctl summary) at info level
  ( export NS_LOG="SpProactiveController=info"
    timeout 8s ./ns3 run "scratch/man_of13_qkd $*" >"$OUTDIR/$name.log" 2>&1
  )
  local rc=$?

  # Capture flows.xml if produced
  if [[ -f flows.xml ]]; then
    cp -f flows.xml "$OUTDIR/$name.flows.xml"
  fi

  # Summarize key lines
  {
    echo "--- $name : EXIT=$rc ---"
    grep -m1 -E "QKD control traffic using" "$OUTDIR/$name.log" || true
    grep -m1 -E "Configured controller with" "$OUTDIR/$name.log" || true
    grep -m1 -E "PfifoFast has no 'Limit' attribute" "$OUTDIR/$name.log" || true
    grep -m1 -E "Installed PfifoFast on all switch ports" "$OUTDIR/$name.log" || true
    grep -m1 -E "MAN with|MAN from CSV" "$OUTDIR/$name.log" || true
    grep -m1 -E "TEARDOWN: .*dpctl" "$OUTDIR/$name.log" || true
    # First few telemetry lines
    grep -E "QSIZE_OBJ|DROP|QLEN" "$OUTDIR/$name.log" | head -10 || true
    # Final totals (if present)
    grep -E "Total BE RX|QKD control RX" "$OUTDIR/$name.log" || true
    echo
  } >> "$OUTDIR/SUMMARY.txt"
}

echo "Building…" | tee "$OUTDIR/SUMMARY.txt"
./ns3 build >> "$OUTDIR/SUMMARY.txt" 2>&1

# Test matrix
run baseline            --nCore=4
run ef_marking          --nCore=4 --qosMark=EF
run qdisc_limits        --nCore=4 --qdiscMaxP=40 --qdiscPollMs=10
run switch_qdisc_stress --nCore=4 --qdiscOnSwitch=1 --beRate=50Mbps --qdiscMaxP=50 --qdiscPollMs=5
run csv_topology        --topo="$OUTDIR/man_topo.csv"
run ring_topology       --nCore=6 --ring=1

# Optional: compress everything for sharing
tar -czf "${OUTDIR}.tgz" "$OUTDIR"
echo "Done. See:"
echo "  $OUTDIR/SUMMARY.txt  (human-readable summaries)"
echo "  ${OUTDIR}.tgz        (full logs and flow XMLs)"
