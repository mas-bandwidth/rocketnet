#!/usr/bin/env bash
# run_bounded.sh <memory KB> <seconds> <command...>: runs a proof under a
# memory limit and a time limit, with the exit statuses the Makefile reads.
#
#   124  the time limit passed
#   137  the memory limit was reached (also printed as "out of memory")
#
# Where the kernel takes an address-space limit (Linux), the limit is
# `ulimit -v` and the time limit is timeout(1). macOS refuses `ulimit -v`, so
# there the command runs under a watch that samples its resident memory every
# two seconds and kills it above the limit.
set -u

if [ "$#" -lt 3 ]; then
    echo "usage: run_bounded.sh <memory KB> <seconds> <command...>" >&2
    exit 2
fi
limit_kb=$1
seconds=$2
shift 2

if ( ulimit -v "$limit_kb" ) 2>/dev/null; then
    ulimit -v "$limit_kb"
    exec timeout "$seconds" "$@"
fi

"$@" &
pid=$!
start=$SECONDS
while kill -0 "$pid" 2>/dev/null; do
    rss_kb=$(ps -o rss= -p "$pid" 2>/dev/null | tr -d ' ')
    if [ -n "$rss_kb" ] && [ "$rss_kb" -gt "$limit_kb" ]; then
        kill -KILL "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
        echo "run_bounded: out of memory: resident $rss_kb KB over the $limit_kb KB limit after $((SECONDS - start)) s"
        exit 137
    fi
    if [ $((SECONDS - start)) -ge "$seconds" ]; then
        kill -TERM "$pid" 2>/dev/null
        sleep 5
        kill -KILL "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
        exit 124
    fi
    sleep 2
done
wait "$pid"
