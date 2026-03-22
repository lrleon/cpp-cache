#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FORMAL_DIR="$ROOT/formal"
MODEL="cache_model.pml"
PAN="./pan"

cd "$FORMAL_DIR"

echo "=== Generating verifier from $MODEL ==="
spin -a "$MODEL"
cc -O2 -DNFAIR=8 -DMEMLIM=4096 -o "$PAN" pan.c

echo ""
echo "=== [1/2] Safety verification (assertions) ==="
"$PAN" -n -E
echo "  Safety: PASSED"

echo ""
echo "=== [2/2] Liveness verification (LTL under weak fairness) ==="
for prop in \
  every_requested_key_eventually_materializes \
  no_starvation_thread0 \
  no_starvation_thread1 \
  no_starvation_thread2 \
  no_starvation_thread3 \
  all_terminate
do
  echo -n "  - $prop ... "
  if "$PAN" -n -E -a -f -N "$prop" 2>&1 | grep -q "errors: 0"; then
    echo "PASSED"
  else
    echo "FAILED"
    "$PAN" -n -E -a -f -N "$prop"
  fi
done

echo ""
echo "=== Cleanup ==="
rm -f pan.* "$PAN" _spin_nvr.tmp
echo "Done."