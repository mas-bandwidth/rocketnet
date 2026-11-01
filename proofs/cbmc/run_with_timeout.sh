#!/usr/bin/env bash
# Nightly opts out of the execution watchdog, not solver/domain/memory checks.
set -eu
if [ "$#" -lt 2 ]; then
    echo "usage: run_with_timeout.sh <seconds> <command...>" >&2
    exit 2
fi
seconds=$1
shift
if [ "${ROCKETNET_NIGHTLY_NO_TIMEOUT:-0}" = "1" ]; then
    exec "$@"
fi
exec timeout "$seconds" "$@"
