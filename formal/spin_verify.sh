#!/usr/bin/env bash
# spin_verify.sh - Wrapper script for SPIN verification of cache_model.pml
# 
# This script provides a simple interface for common SPIN tasks:
# - Safety check (assertions and deadlocks)
# - LTL verification (liveness properties)
# - Random simulation
#
# Usage:
#   ./spin_verify.sh safety
#   ./spin_verify.sh ltl <property_name>
#   ./spin_verify.sh sim
#   ./spin_verify.sh all

set -euo pipefail

MODEL="cache_model.pml"
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PAN="$DIR/pan"

cd "$DIR"

# Cleanup function
cleanup() {
    rm -f pan.* "$PAN" _spin_nvr.tmp
}

# Compile the verifier
compile_pan() {
    echo "--- Compiling verifier for $MODEL ---"
    spin -a "$MODEL"
    # -DNFAIR=8: use weak fairness (required for liveness properties)
    # -DMEMLIM=4096: allow up to 4GB of RAM
    # -O2: optimization for speed
    cc -O2 -DNFAIR=8 -DMEMLIM=4096 -o "$PAN" pan.c
}

run_safety() {
    compile_pan
    echo "--- Running Safety Check (Assertions & Deadlocks) ---"
    # -m100000: max search depth
    # -n: suppress some output
    # -E: ignore invalid end states (expected for background processes)
    "$PAN" -m100000 -n -E
    cleanup
}

run_ltl() {
    local prop=$1
    compile_pan
    echo "--- Running LTL Verification for: $prop ---"
    # -a: search for acceptance cycles
    # -N: specify the never claim (LTL property) name
    if "$PAN" -m100000 -n -E -a -N "$prop"; then
        echo "LTL property '$prop' PASSED."
    else
        echo "LTL property '$prop' FAILED."
    fi
    cleanup
}

run_sim() {
    echo "--- Running Random Simulation ---"
    # -p: print all statements
    # -g: print global variables
    # -l: print local variables
    spin -p -g -l "$MODEL"
}

case "${1:-}" in
    safety)
        run_safety
        ;;
    ltl)
        if [[ -z "${2:-}" ]]; then
            echo "Error: Please specify an LTL property name."
            echo "Available properties: every_requested_key_eventually_materializes, all_terminate, no_starvation_thread[0-5]"
            exit 1
        fi
        run_ltl "$2"
        ;;
    sim)
        run_sim
        ;;
    all)
        ./run_spin.sh
        ;;
    *)
        echo "Usage: $0 {safety|ltl <prop>|sim|all}"
        echo ""
        echo "Examples:"
        echo "  $0 safety"
        echo "  $0 ltl all_terminate"
        echo "  $0 sim"
        exit 1
        ;;
esac
