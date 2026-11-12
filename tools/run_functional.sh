#!/usr/bin/env bash
# Run one whole functional suite and hold its skips to the leg's list.
#
#   tools/run_functional.sh <variant> [--log FILE] <suite command...>
#
# Runs the command with stdout and stderr through tee into FILE (default
# functional-<variant>.log, carriage returns removed), then
# tools/check_skips.py --variant <variant> on it: the leg is <os>-<variant>,
# and every SKIPPED line must be one tests/functional/expected_skips.txt lists
# for it, every listed row must skip, and the suite must have printed its
# "skipped: <n>" line. The exit is the suite's when the suite failed, else the
# check's, so a red suite is never reported as a skip problem and a green
# suite with the wrong skips is red. Every whole-suite run in a workflow goes
# through here (check_skips.py --lint fails on one that does not).
set -uo pipefail

if [ $# -lt 2 ]; then
    echo "usage: tools/run_functional.sh <variant> [--log FILE] <suite command...>" >&2
    exit 2
fi
here=$(cd "$(dirname "$0")" && pwd)
variant=$1
shift
log="functional-$variant.log"
if [ "$1" = "--log" ]; then
    log=$2
    shift 2
fi
if [ $# -lt 1 ]; then
    echo "run_functional.sh: no suite command" >&2
    exit 2
fi

"$@" 2>&1 | tr -d '\r' | tee "$log"
suite=${PIPESTATUS[0]}

python=python3
if [ "${OS:-}" = "Windows_NT" ] || ! command -v python3 >/dev/null 2>&1; then
    python=python
fi
"$python" "$here/check_skips.py" --variant "$variant" "$log"
skips=$?

if [ "$suite" -ne 0 ]; then
    echo "run_functional.sh: the suite exited $suite" >&2
    exit "$suite"
fi
exit "$skips"
