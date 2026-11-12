#!/bin/sh
# check_reachability_control.sh <cbmc|esbmc> <harness> <checker exit status> <checker output>
#
# Judges one reader harness's reachability control: the harness rebuilt with
# -DREACHABILITY_CONTROL, whose last statement is
# PROOF_REACHED( "<harness>" ), an assertion that must fail. A harness whose
# assumptions became unsatisfiable proves every property and never reaches
# that line, so its proof passes vacuously and only this control goes red.
#
# cbmc: exit 10, VERIFICATION FAILED, and exactly one failed property, the
#       control's. esbmc (which stops at the first violation): exit 1,
#       VERIFICATION FAILED, and the violated property is the control's.
# Prints "<harness>-reachability EXPECTED FAILURE", or a HOLD line, the
# output, and exits 1.

set -u

tool="$1"
harness="$2"
status="$3"
output="$4"
name="$harness reachability control"

hold() {
    cat "$output"
    echo "$harness-reachability HOLD: $1 (exit $status)"
    exit 1
}

grep -q "^VERIFICATION FAILED" "$output" || hold "the control assertion was never reached"

case "$tool" in
    cbmc)
        [ "$status" -eq 10 ] || hold "cbmc did not report a failure"
        failures=$(grep -c ': FAILURE$' "$output")
        [ "$failures" -eq 1 ] || hold "expected one failed property, the control's; saw $failures"
        grep ': FAILURE$' "$output" | grep -q "$name" || hold "the failed property is not the control"
        ;;
    esbmc)
        [ "$status" -eq 1 ] || hold "esbmc did not report a failure"
        grep -A4 "^Violated property" "$output" | grep -q "$name" || hold "the violated property is not the control"
        ;;
    *)
        echo "check_reachability_control.sh: unknown checker $tool" >&2
        exit 2
        ;;
esac

echo "$harness-reachability EXPECTED FAILURE"
