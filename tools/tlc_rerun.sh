#!/usr/bin/env bash
# Re-run committed TLA+ cases and hold each fresh result to its RUNS.tsv
# record: tlc.yml's `TLC re-run slice K/N` and `TLC re-run full` jobs, and the
# same command on a bench.
#
#   tools/tlc_rerun.sh <job-dir> <parallel> --day <UTC day number> | --full
#
# Needs TLC_NIGHTLY (the adapter built from tools/tlc_nightly.go with its
# source SHA-256) and TLC_JAR (the pinned tla2tools.jar). Writes the plan, each
# case's evidence (run-<group>-<shard>/result.json and tlc.log) and the
# comparison into <job-dir>. Red when the plan is empty, when any case does not
# reach its declared result, or when any fresh result differs from its record
# (tools/compare_tlc_rerun.py names the case and the field).
set -euo pipefail

if [ "$#" -lt 3 ]; then
  echo "usage: tools/tlc_rerun.sh <job-dir> <parallel> --day <n> | --full" >&2
  exit 2
fi
job="$1"
parallel="$2"
shift 2
: "${TLC_NIGHTLY:?TLC_NIGHTLY must name the built adapter}"
: "${TLC_JAR:?TLC_JAR must name the pinned tla2tools.jar}"
mkdir -p "$job"

# A planner refusal (an empty slice, a bad record) stops here, red.
python3 tools/plan_tlc_nightly.py --select rerun "$@" > "$job/rerun-plan" 2> "$job/rerun-plan.log" \
  || { cat "$job/rerun-plan.log"; exit 1; }
grep -v '^TLC-PLAN ' "$job/rerun-plan.log" || [ $? -eq 1 ]
echo "TLC-RERUN parallel=$parallel cases: $(cut -d' ' -f4 "$job/rerun-plan" | tr '\n' ' ')"

export TLC_JOB="$job"
: > "$job/rerun-failed"
# shellcheck disable=SC2016  # the shard's own shell expands these
xargs -P "$parallel" -L 1 bash -c '
  dir="$TLC_JOB/run-$0-$2"
  "$TLC_NIGHTLY" --root proofs --jar "$TLC_JAR" --group "$0" --shards "$1" --shard "$2" --dir "$dir" > "$dir.log" 2>&1 \
    || echo "$3" >> "$TLC_JOB/rerun-failed"
' < "$job/rerun-plan"

# Every planned case is compared, whether or not its adapter run passed: the
# comparison names each difference by case and field.
status=0
python3 tools/compare_tlc_rerun.py --plan "$job/rerun-plan" --evidence "$job" | tee "$job/rerun-compare.log" || status=1
if [ -s "$job/rerun-failed" ]; then
  echo "TLC-RERUN adapter refused or did not reach the declared result: $(tr '\n' ' ' < "$job/rerun-failed")"
  status=1
fi
exit "$status"
