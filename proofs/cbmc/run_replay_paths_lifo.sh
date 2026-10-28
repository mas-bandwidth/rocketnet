#!/usr/bin/env bash
# One full-domain CBMC path-exploration attempt for the reviewed nonwrapping
# replay wrapper. A timeout or any nonzero checker exit remains incomplete.

set -u
set -o pipefail

if [ "$#" -ne 3 ] || [ -z "$1" ] || [ -z "$2" ] || [ -z "$3" ]; then
    echo "usage: $0 <new-log-path> <full-source-head> <source-tree>" >&2
    exit 2
fi

log=$1
proof_head=$2
proof_tree=$3
if [ -e "$log" ]; then
    echo "refusing to overwrite existing log: $log" >&2
    exit 2
fi

{
    echo "PROOF_KIND=cbmc-full-domain-path-exploration"
    echo "PROOF_HEAD=$proof_head"
    echo "PROOF_TREE=$proof_tree"
    echo "START_UTC=$(date -u +%FT%TZ)"
    echo "HOST=$(hostname)"
    uname -a
    echo "NPROC=$(nproc)"
    grep '^MemTotal:' /proc/meminfo
    /usr/bin/cbmc --version
    sha256sum /usr/bin/cbmc proof_replay_advance.c ../../sdk/rocketnet_replay.cpp
    echo "BOUND=delta-1..4095 partition=nonwrapping unwind=65 paths=lifo"
    echo "CHECKS=bounds,pointer,signed-overflow,unsigned-overflow,division-by-zero,unwinding-assertions"
    echo "LIMITS=wall-1800s virtual-memory-16000000KiB"
    echo "COMMAND=/usr/bin/cbmc --bounds-check --pointer-check --signed-overflow-check --unsigned-overflow-check --div-by-zero-check --unwinding-assertions --unwind 65 -I../../sdk --paths lifo -DREPLAY_DELTA_BOUND=4096 --function harness_replay_near_nonwrapping proof_replay_advance.c"

    ulimit -v 16000000
    /usr/bin/time -v timeout 1800 /usr/bin/cbmc \
        --bounds-check \
        --pointer-check \
        --signed-overflow-check \
        --unsigned-overflow-check \
        --div-by-zero-check \
        --unwinding-assertions \
        --unwind 65 \
        -I../../sdk \
        --paths lifo \
        -DREPLAY_DELTA_BOUND=4096 \
        --function harness_replay_near_nonwrapping \
        proof_replay_advance.c
    checker_exit=$?
    echo "CHECKER_DIRECT_EXIT=$checker_exit"
    echo "END_UTC=$(date -u +%FT%TZ)"
    exit "$checker_exit"
} 2>&1 | tee "$log"

outer_exit=${PIPESTATUS[0]}
echo "PROOF_OUTER_EXIT=$outer_exit" | tee -a "$log"
exit "$outer_exit"
