#!/usr/bin/env bash
# RocketNet. Copyright 2026 Más Bandwidth LLC.
# Licensed under the GNU Affero General Public License v3.0
#
# Checks that this user's RLIMIT_MEMLOCK covers the io_uring functional cases
# every CI runner of this user on this host can run at once
# (docs/LINUX.md sections 5 and 8, tests/suite_runner.h).
#
#   bash tools/check_memlock.sh [path to transport_memory]
#
# The io_uring transport never raises RLIMIT_MEMLOCK, and on kernel 7.0 the
# kernel charges every ring's locked pages to the user, across all of that
# user's processes, against the creating process's limit. Each runner runs one
# job, and a job's functional suite runs at most one io_uring case at a time
# (tests/suite_runner.h), so the user needs
#
#     limit >= runners * per-case need
#
# where runners is the count of this user's Runner.Listener processes (at
# least 1) and the per-case need is the suite runner's own value for this
# kernel, read from tests/suite_runner.h (ROCKETNET_TEST_URING_CASE_MEMLOCK_KIB
# overrides it, as it does the suite's). It also measures the per-server need
# on this kernel: the smallest `ulimit -l` (16 KiB steps, under this limit) at
# which ./bin/transport_memory creates a default 4-queue io_uring server (4
# clients: the ring and the provided buffers are fixed per queue and do not
# grow with max_clients), and fails when that
# is over the per-case need (the suite runner's value is stale for this
# kernel). It prints the limit, both needs, the runners and PASS, or FAIL and
# exits 1. Creating the measuring server opens a loopback socket; the script
# changes no system setting.

set -u

here=$(cd "$(dirname "$0")/.." && pwd)
probe=${1:-$here/bin/transport_memory}

if [ "$(uname -s)" != Linux ]; then
    echo "memlock: not Linux, no io_uring transport: PASS"
    exit 0
fi
disabled=$(cat /proc/sys/kernel/io_uring_disabled 2>/dev/null || echo 0)
if [ "$disabled" != 0 ]; then
    echo "memlock: kernel.io_uring_disabled=$disabled, the io_uring cases do not create rings: PASS"
    exit 0
fi

limit=$(ulimit -l)
kernel=$(uname -r)

# the per-case need: the suite runner's #define for this kernel
read_define() { sed -n "s/^#define $1 \([0-9][0-9]*\).*/\1/p" "$here/tests/suite_runner.h"; }
major=${kernel%%.*}
rest=${kernel#*.}
minor=${rest%%[!0-9]*}
if [ "$major" -lt 6 ] || { [ "$major" -eq 6 ] && [ "$minor" -le 8 ]; }; then
    case_kib=$(read_define ROCKETNET_TEST_URING_CASE_MEMLOCK_KIB_6_8)
else
    case_kib=$(read_define ROCKETNET_TEST_URING_CASE_MEMLOCK_KIB)
fi
if [ -n "${ROCKETNET_TEST_URING_CASE_MEMLOCK_KIB:-}" ] && [ "$ROCKETNET_TEST_URING_CASE_MEMLOCK_KIB" -gt 0 ] 2>/dev/null; then
    case_kib=$ROCKETNET_TEST_URING_CASE_MEMLOCK_KIB
fi
if [ -z "$case_kib" ]; then
    echo "memlock: FAIL: no per-case need in $here/tests/suite_runner.h"
    exit 1
fi

runners=$(pgrep -u "$(id -u)" -c -f Runner.Listener 2>/dev/null || true)
[ -n "$runners" ] && [ "$runners" -ge 1 ] || runners=1

echo "memlock: user $(id -un), kernel $kernel, RLIMIT_MEMLOCK soft $limit KiB, hard $(ulimit -H -l) KiB"

if [ "$limit" = unlimited ]; then
    echo "memlock: per-case need $case_kib KiB, runners $runners, limit unlimited: PASS"
    exit 0
fi

creates() { bash -c "ulimit -l $1 && \"$probe\" 4 1416 io_uring 0 >/dev/null 2>&1"; }
server_kib="not measured (no $probe)"
if [ -x "$probe" ]; then
    if ! creates "$limit"; then
        echo "memlock: per-server need: a default io_uring server does not create under $limit KiB on this kernel"
        echo "memlock: FAIL: raise LimitMEMLOCK (docs/LINUX.md section 8)"
        exit 1
    fi
    lo=0
    hi=$limit
    while [ $((hi - lo)) -gt 16 ]; do
        mid=$(( (lo + hi) / 2 ))
        if creates "$mid"; then hi=$mid; else lo=$mid; fi
    done
    server_kib="$hi KiB"
    if [ "$hi" -gt "$case_kib" ]; then
        echo "memlock: per-server need $hi KiB is over the per-case need $case_kib KiB in tests/suite_runner.h"
        echo "memlock: FAIL: measure the io_uring cases on this kernel and raise the per-case need"
        exit 1
    fi
fi

need=$(( runners * case_kib ))
echo "memlock: per-server need $server_kib, per-case need $case_kib KiB, runners $runners, need $runners * $case_kib = $need KiB"
if [ "$limit" -lt "$need" ]; then
    echo "memlock: FAIL: $limit KiB is under $need KiB; set LimitMEMLOCK for the runner services and the user manager (docs/LINUX.md section 8)"
    exit 1
fi
echo "memlock: $limit KiB covers $need KiB: PASS"
exit 0
