#!/usr/bin/env bash
# verify_full.sh - Comprehensive SPIN verification with detailed metrics
#
# Usage: ./verify_full.sh [MODE] [OPTIONS]
#
# Modes:
#   safety          Only safety properties (assertions, deadlocks)
#   ltl             Only LTL properties
#   all             Both safety and LTL (default)
#
# Options:
#   -m, --mem MB    Memory limit in MB (default: 4096)
#   -d, --depth N   Max search depth (default: 200000)
#   --collapse      Use COLLAPSE compression (less RAM, slower)
#   --fair N        Weak fairness steps (default: 8)
#   -q, --quiet     Suppress progress output
#
# Examples:
#   ./verify_full.sh ltl --collapse --mem 16384
#   ./verify_full.sh safety --mem 8192
#   ./verify_full.sh all --collapse -q
#
# Each search is reported as PASS (full state space explored, no errors), FAIL
# (errors found) or INCOMPLETE (stopped by the memory bound, the depth bound or
# an interruption; "errors: 0" then only covers the explored part). pan exits
# with status 0 in all three cases, so the status is read from its output.
#
# Exit status: 0 if every search is PASS, 1 if any is FAIL, 2 if some are
# INCOMPLETE and none FAIL.

set -euo pipefail

MODEL="cache_model.pml"
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PAN="$DIR/pan"
LOG="$DIR/verify_output.log"

# Defaults
MEM_LIMIT=4096
MAX_DEPTH=200000
NFAIR=8
COLLAPSE=0
QUIET=0

# Parse arguments
MODE=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        safety|ltl|all)
            MODE="$1" ;;
        -m|--mem)
            MEM_LIMIT="$2"; shift ;;
        -d|--depth)
            MAX_DEPTH="$2"; shift ;;
        --collapse)
            COLLAPSE=1 ;;
        --fair)
            NFAIR="$2"; shift ;;
        -q|--quiet)
            QUIET=1 ;;
        -h|--help)
            sed -n '2,/^$/{ s/^# \?//; p }' "$0"
            exit 0 ;;
        *)
            echo "Unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done
MODE="${MODE:-all}"

cd "$DIR"

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
BLUE='\033[0;34m'
YELLOW='\033[0;33m'
BOLD='\033[1m'
NC='\033[0m'

# Build compiler flags
CC_FLAGS="-O3 -DNFAIR=$NFAIR -DMEMLIM=$MEM_LIMIT"
if [[ $COLLAPSE -eq 1 ]]; then
    CC_FLAGS="$CC_FLAGS -DCOLLAPSE"
fi

echo -e "${BLUE}${BOLD}=== SPIN COMPREHENSIVE VERIFICATION ===${NC}"
echo -e "Model: $MODEL"
echo -e "Mode:  $MODE"
echo -e "Mem:   ${MEM_LIMIT} MB | Depth: ${MAX_DEPTH} | Fair: ${NFAIR}$(if [[ $COLLAPSE -eq 1 ]]; then echo ' | COLLAPSE'; fi)"
echo -e "Time:  $(date)"
echo "----------------------------------------"

# Cleanup
rm -f "$LOG" pan.* "$PAN" _spin_nvr.tmp

# Run pan and optionally show progress on the same terminal line
run_pan() {
    local log=$1
    shift
    if [[ $QUIET -eq 0 ]] && [[ -t 1 ]]; then
        "$PAN" "$@" 2>&1 | tee "$log" | \
            grep --line-buffered "^Depth=" | \
            while IFS= read -r line; do
                printf "\r  ${YELLOW}%s${NC}" "$line " >&2
            done
        local rc=${PIPESTATUS[0]}
        printf "\r\033[K" >&2
        return $rc
    else
        "$PAN" "$@" > "$log" 2>&1
    fi
}

# Function to parse and print pan output
print_stats() {
    local label=$1
    local file=$2
    local exit_code=$3

    local states=$(grep "states, stored" "$file" | awk '{print $1}' || echo "0")
    local depth=$(grep -oP 'depth reached \K[0-9]+' "$file" || echo "0")
    local memory=$(grep "total actual memory usage" "$file" | awk '{print $1}' || echo "0")
    local time=$(grep -oP 'elapsed time \K[0-9.]+' "$file" || echo "0")
    local errors=$(grep -oP 'errors: \K[0-9]+' "$file" | tail -n 1 || true)
    local incomplete=$(grep -c -E "Search not completed|reached -DMEMLIM bound|max search depth too small|^Interrupted" "$file" || true)

    local status="${GREEN}PASS${NC}"
    local rc=0
    if [[ -n "$errors" && "$errors" != "0" ]]; then
        status="${RED}FAIL${NC}"
        rc=1
    elif [[ -z "$errors" || "$incomplete" -gt 0 || $exit_code -ne 0 ]]; then
        # No final report, a bound reached or an interruption: the search did not finish.
        status="${BLUE}INCOMPLETE${NC}"
        rc=2
    fi

    printf "%-45s | ${status} | %10s st | %6s dp | %7s MB | %5s s\n" "$label" "$states" "$depth" "$memory" "$time"
    return $rc
}

print_header() {
    printf "\n${BOLD}%-45s | %-12s | %12s | %8s | %9s | %7s${NC}\n" "PROPERTY" "STATUS" "STATES" "DEPTH" "MEM" "TIME"
    echo "----------------------------------------------------------------------------------------------------"
}

# Count FAIL (status 1) and INCOMPLETE (status 2) searches
FAILED_COUNT=0
INCOMPLETE_COUNT=0
tally() {
    case "$1" in
        1) FAILED_COUNT=$((FAILED_COUNT + 1)) ;;
        2) INCOMPLETE_COUNT=$((INCOMPLETE_COUNT + 1)) ;;
    esac
}

# 1. Safety Check (if mode is safety or all)
if [[ "$MODE" == "safety" || "$MODE" == "all" ]]; then
    echo -n "Compiling safety verifier... "
    # -DNOCLAIM: check the assertions on their own; otherwise pan would run them
    # inside the first LTL claim of the model.
    if spin -a "$MODEL" && cc $CC_FLAGS -DSAFETY -DNOCLAIM -o "$PAN" pan.c 2>>"$LOG"; then
        echo -e "${GREEN}DONE${NC}"
        print_header
        run_pan "$LOG" -m$MAX_DEPTH -n -E && pan_rc=0 || pan_rc=$?
        print_stats "Safety (assertions, no claim)" "$LOG" $pan_rc && st=0 || st=$?
        tally $st
        echo "----------------------------------------------------------------------------------------------------"
    else
        echo -e "${RED}FAILED${NC} (check $LOG)"
        exit 1
    fi
fi

# 2. LTL Properties (if mode is ltl or all)
if [[ "$MODE" == "ltl" || "$MODE" == "all" ]]; then
    echo -n "Compiling LTL verifier... "
    if spin -a "$MODEL" && cc $CC_FLAGS -o "$PAN" pan.c >>"$LOG" 2>&1; then
        echo -e "${GREEN}DONE${NC}"

        # Header if only LTL mode
        if [[ "$MODE" == "ltl" ]]; then
            print_header
        fi

        # Every ltl block declared in the model
        mapfile -t PROPS < <(sed -n 's/^[[:space:]]*ltl[[:space:]]\{1,\}\([A-Za-z_][A-Za-z0-9_]*\).*/\1/p' "$MODEL")

        for prop in "${PROPS[@]}"; do
            run_pan "$LOG" -m$MAX_DEPTH -n -E -a -f -N "$prop" && pan_rc=0 || pan_rc=$?
            print_stats "LTL: $prop" "$LOG" $pan_rc && st=0 || st=$?
            tally $st
        done
        echo "----------------------------------------------------------------------------------------------------"
    else
        echo -e "${RED}FAILED${NC} (check $LOG)"
        exit 1
    fi
fi

if [ $FAILED_COUNT -gt 0 ]; then
    echo -e "${RED}${BOLD}VERDICT: FAIL ($FAILED_COUNT searches found errors)${NC}"
    echo -e "Check $LOG for details or run 'spin -t -p $MODEL' for replay."
    VERDICT_RC=1
elif [ $INCOMPLETE_COUNT -gt 0 ]; then
    echo -e "${BLUE}${BOLD}VERDICT: INCOMPLETE ($INCOMPLETE_COUNT searches stopped before exhausting the state space)${NC}"
    echo -e "Their \"errors: 0\" only covers the explored part. Raise --mem, use --collapse or reduce the model bounds."
    VERDICT_RC=2
else
    echo -e "${GREEN}${BOLD}VERDICT: PASS (every search explored the full state space of the bounded model without errors)${NC}"
    VERDICT_RC=0
fi

# Cleanup
rm -f pan.* "$PAN" _spin_nvr.tmp
exit $VERDICT_RC
