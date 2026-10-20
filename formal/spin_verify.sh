#!/usr/bin/env bash
# spin_verify.sh - Shortcuts for common SPIN tasks on cache_model.pml
#
# Usage:
#   ./spin_verify.sh safety            assertion search, without never claims
#   ./spin_verify.sh ltl <property>    one LTL property under weak fairness
#   ./spin_verify.sh all               assertions and every LTL property
#   ./spin_verify.sh list              LTL properties declared in the model
#   ./spin_verify.sh sim               random simulation
#
# safety, ltl and all delegate to ./run_spin.sh, which reports PASS, FAIL or
# INCOMPLETE for each search, accepts the same environment variables (MEMLIM,
# DEPTH, NFAIR, COLLAPSE, TIMEOUT, OUT_DIR) and uses the same exit status:
# 0 all PASS, 1 any FAIL, 2 some INCOMPLETE, 3 usage or tool error.

set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODEL_FILE="$DIR/cache_model.pml"

list_props() {
    sed -n 's/^[[:space:]]*ltl[[:space:]]\{1,\}\([A-Za-z_][A-Za-z0-9_]*\).*/\1/p' "$MODEL_FILE"
}

usage() {
    echo "Usage: $0 {safety|ltl <property>|all|list|sim}"
    echo ""
    echo "Examples:"
    echo "  $0 safety"
    echo "  $0 ltl all_terminate"
    echo "  MEMLIM=16384 $0 all"
    echo "  $0 sim"
}

case "${1:-}" in
    safety)
        exec env SAFETY=1 PROPS=none "$DIR/run_spin.sh"
        ;;
    ltl)
        if [[ -z "${2:-}" ]]; then
            echo "Error: please specify an LTL property name. Declared in the model:" >&2
            list_props | sed 's/^/  /' >&2
            exit 3
        fi
        exec env SAFETY=0 PROPS="$2" "$DIR/run_spin.sh"
        ;;
    all)
        exec "$DIR/run_spin.sh"
        ;;
    list)
        list_props
        ;;
    sim)
        cd "$DIR"
        # -p: print all statements, -g: print global variables, -l: print local variables
        exec spin -p -g -l "$(basename "$MODEL_FILE")"
        ;;
    *)
        usage
        exit 3
        ;;
esac
