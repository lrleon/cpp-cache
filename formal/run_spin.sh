#!/usr/bin/env bash
# run_spin.sh: verify formal/cache_model.pml with SPIN and report an honest verdict.
#
# Runs the assertion search without never claims (-DSAFETY -DNOCLAIM) and every
# LTL property declared in the model under weak fairness, and classifies each search:
#
#   PASS        pan explored the whole state space and reported no errors
#   FAIL        pan reported errors; the counterexample trail is kept
#   INCOMPLETE  pan stopped before exhausting the state space (memory bound, depth
#               bound, time limit, or no final report); its "errors: 0" only
#               covers the part of the state space that was explored
#
# pan exits with status 0 in all three cases, so the verdict is read from its output.
#
# Environment variables [defaults]:
#   MEMLIM    memory bound for pan, in MB [4096]
#   DEPTH     maximum search depth [100000]
#   NFAIR     weak-fairness bound for the LTL searches [8]
#   COLLAPSE  1 to compile with -DCOLLAPSE (lossless state compression), 0 to disable [1]
#   TIMEOUT   wall-clock limit per search, in seconds; 0 means no limit [0]
#   SAFETY    1 to run the assertion search, 0 to skip it [1]
#   PROPS     LTL properties to check: "all", "none" or a space-separated list [all]
#   OUT_DIR   directory for pan outputs, trails and summary.tsv [a new temporary directory]
#   MODEL     Promela model to verify [formal/cache_model.pml]
#
# Exit status: 0 if every search is PASS, 1 if any search is FAIL,
#              2 if none failed but at least one is INCOMPLETE, 3 on usage or tool errors.
#
# With the default 4 GB bound no search of the v5 model completes; see formal/README.md.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODEL="${MODEL:-$ROOT/formal/cache_model.pml}"
MEMLIM="${MEMLIM:-4096}"
DEPTH="${DEPTH:-100000}"
NFAIR="${NFAIR:-8}"
COLLAPSE="${COLLAPSE:-1}"
TIMEOUT="${TIMEOUT:-0}"
SAFETY="${SAFETY:-1}"
PROPS="${PROPS:-all}"

die() {
  echo "run_spin.sh: $*" >&2
  exit 3
}

for tool in spin cc; do
  command -v "$tool" >/dev/null 2>&1 || die "'$tool' not found in PATH"
done
[[ -f "$MODEL" ]] || die "model not found: $MODEL"
for var in MEMLIM DEPTH NFAIR TIMEOUT; do
  [[ "${!var}" =~ ^[0-9]+$ ]] || die "$var must be a non-negative integer"
done
if [[ "$TIMEOUT" != 0 ]]; then
  command -v timeout >/dev/null 2>&1 || die "TIMEOUT needs the 'timeout' command"
fi

OUT_DIR="${OUT_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/spin-results.XXXXXX")}"
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/spin-work.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

MODEL_NAME="$(basename "$MODEL")"
cp "$MODEL" "$WORK/$MODEL_NAME"
cp "$MODEL" "$OUT_DIR/$MODEL_NAME"
cd "$WORK"

declared=()
mapfile -t declared < <(sed -n 's/^[[:space:]]*ltl[[:space:]]\{1,\}\([A-Za-z_][A-Za-z0-9_]*\).*/\1/p' "$MODEL_NAME")

props=()
case "$PROPS" in
  all) props=("${declared[@]}") ;;
  none) ;;
  *) read -r -a props <<<"$PROPS" ;;
esac
for prop in "${props[@]}"; do
  printf '%s\n' "${declared[@]}" | grep -qx -- "$prop" || die "unknown LTL property '$prop'; declared: ${declared[*]}"
done
if [[ "$SAFETY" != 1 && ${#props[@]} -eq 0 ]]; then
  die "nothing to verify: SAFETY=0 and no LTL properties selected"
fi

cflags=(-O2 "-DMEMLIM=$MEMLIM")
if [[ "$COLLAPSE" == 1 ]]; then
  cflags+=(-DCOLLAPSE)
fi

echo "=== SPIN verification of $MODEL_NAME ==="
echo "MEMLIM=$MEMLIM MB  DEPTH=$DEPTH  NFAIR=$NFAIR  COLLAPSE=$COLLAPSE  TIMEOUT=$TIMEOUT s"
echo "Full pan outputs: $OUT_DIR"
echo "Each search can take many minutes and the whole run can take hours; pan prints nothing until a search ends."
echo

spin -a "$MODEL_NAME" >"$OUT_DIR/spin-a.txt" 2>&1 || {
  cat "$OUT_DIR/spin-a.txt" >&2
  die "spin -a failed"
}
if [[ "$SAFETY" == 1 ]]; then
  cc "${cflags[@]}" -DSAFETY -DNOCLAIM -o pan_safety pan.c 2>"$OUT_DIR/cc-safety.txt" || {
    cat "$OUT_DIR/cc-safety.txt" >&2
    die "compiling the assertion verifier failed"
  }
fi
if [[ ${#props[@]} -gt 0 ]]; then
  cc "${cflags[@]}" "-DNFAIR=$NFAIR" -o pan_ltl pan.c 2>"$OUT_DIR/cc-ltl.txt" || {
    cat "$OUT_DIR/cc-ltl.txt" >&2
    die "compiling the LTL verifier failed"
  }
fi

printf 'search\tstatus\tstates_stored\tdepth\tmemory_mb\ttime_s\tnotes\n' >"$OUT_DIR/summary.tsv"
rows=()
n_fail=0
n_incomplete=0

# run_search LABEL SLUG COMMAND...: runs one pan search and records its verdict.
run_search() {
  local label=$1 slug=$2
  shift 2
  local out="$OUT_DIR/$slug.txt" rc=0
  printf '%-50s ' "$label"
  if [[ "$TIMEOUT" != 0 ]]; then
    timeout --signal=INT --kill-after=30 "$TIMEOUT" "$@" >"$out" 2>&1 || rc=$?
  else
    "$@" >"$out" 2>&1 || rc=$?
  fi

  local errors status notes=()
  errors="$(sed -n 's/.*errors: \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)"
  if [[ -n "$errors" && "$errors" != 0 ]]; then
    status=FAIL
    notes+=("$errors error(s)")
    if [[ -f "$MODEL_NAME.trail" ]]; then
      mv "$MODEL_NAME.trail" "$OUT_DIR/$slug.trail"
      notes+=("trail $slug.trail")
    fi
  else
    if [[ -z "$errors" ]]; then
      notes+=("no final report from pan, exit status $rc")
    fi
    if grep -q 'reached -DMEMLIM bound' "$out"; then
      notes+=("memory bound of $MEMLIM MB reached")
    fi
    if grep -q 'max search depth too small' "$out"; then
      notes+=("depth bound of $DEPTH reached")
    fi
    if [[ $rc -eq 124 ]] || grep -q '^Interrupted' "$out"; then
      notes+=("time limit of $TIMEOUT s reached")
    fi
    if [[ ${#notes[@]} -eq 0 ]] && grep -q 'Search not completed' "$out"; then
      notes+=("search not completed")
    fi
    if [[ ${#notes[@]} -gt 0 ]]; then
      status=INCOMPLETE
    else
      status=PASS
    fi
  fi

  case $status in
    FAIL) n_fail=$((n_fail + 1)) ;;
    INCOMPLETE) n_incomplete=$((n_incomplete + 1)) ;;
  esac

  local states depth memory seconds note_text
  states="$(sed -n 's/^ *\([0-9][0-9.e+]*\) states, stored.*/\1/p' "$out" | tail -n 1)"
  depth="$(sed -n 's/.*depth reached \([0-9][0-9]*\).*/\1/p' "$out" | tail -n 1)"
  memory="$(sed -n 's/^ *\([0-9][0-9.]*\)[[:space:]]*total actual memory usage.*/\1/p' "$out" | tail -n 1)"
  seconds="$(sed -n 's/^pan: elapsed time \([0-9][0-9.e+]*\) seconds.*/\1/p' "$out" | tail -n 1)"
  note_text="$(IFS=';'; echo "${notes[*]:-}")"
  note_text="${note_text//;/; }"

  echo "$status${note_text:+ ($note_text)}"
  rows+=("$(printf '%-50s %-11s %15s %7s %11s %9s' "$label" "$status" "${states:--}" "${depth:--}" "${memory:--}" "${seconds:--}")")
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$slug" "$status" "$states" "$depth" "$memory" "$seconds" "$note_text" >>"$OUT_DIR/summary.tsv"
}

if [[ "$SAFETY" == 1 ]]; then
  echo "--- Assertions (no never claim) ---"
  run_search "assertions" safety ./pan_safety "-m$DEPTH" -n -E
  echo
fi
if [[ ${#props[@]} -gt 0 ]]; then
  echo "--- LTL properties under weak fairness ---"
  for prop in "${props[@]}"; do
    run_search "$prop" "ltl-$prop" ./pan_ltl "-m$DEPTH" -n -E -a -f -N "$prop"
  done
  echo
fi

echo "=== Summary ==="
printf '%-50s %-11s %15s %7s %11s %9s\n' "search" "status" "states stored" "depth" "memory MB" "time s"
printf '%s\n' "${rows[@]}"
echo

total=${#rows[@]}
if [[ $n_fail -gt 0 ]]; then
  echo "Verdict: FAIL. $n_fail of $total searches found errors. To replay a trail:"
  echo "  cd $OUT_DIR && spin -t -k <search>.trail -p $MODEL_NAME"
  exit 1
elif [[ $n_incomplete -gt 0 ]]; then
  echo "Verdict: INCOMPLETE. $n_incomplete of $total searches stopped before exhausting the state space;"
  echo "their \"errors: 0\" only covers the part that was explored. Raise MEMLIM (and TIMEOUT"
  echo "if set), keep COLLAPSE=1, or reduce the model bounds; see formal/README.md."
  exit 2
else
  echo "Verdict: PASS. All $total searches explored the full state space of the bounded model without errors."
  exit 0
fi
