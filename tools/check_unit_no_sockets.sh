#!/usr/bin/env bash
# The unit suite opens no socket (the owner's rule: unit tests use no sockets,
# no network and no outside services). Runs the unit binary under
# strace --seccomp-bpf -f -e trace=%network,execve and exits nonzero, naming the first caller
# (the case that ran it, read from the worker's execve, and the call),
# when any process of the suite makes any network system call: socket,
# socketpair, bind, listen, accept, connect, send/recv in every form,
# shutdown, the socket options and names (strace's %network class). Tracing
# socket() alone passed a socketpair() exchanging a datagram (VERIFY-3 W8).
# execve is traced only to name the case. A failing suite fails the check too, and the suite's own output
# prints, so on Linux CI this run is the plain unit run as well. Zero inputs is
# an error: a run in which no case worker was started (the runner spawns each
# case as `<binary> --run <case>`) checked nothing and fails.
#
# usage: tools/check_unit_no_sockets.sh [binary [suite arguments...]]   (default ./bin/test)
#
# Arguments after the binary go to it, so a shard runs as
# tools/check_unit_no_sockets.sh ./bin/test --shard 0/2.

set -eu

binary=${1:-./bin/test}
if [ "$#" -gt 0 ]; then
    shift
fi

if ! command -v strace >/dev/null 2>&1; then
    echo "TOOL-MISSING strace (any) (missing)"
    exit 2
fi
if [ ! -x "$binary" ]; then
    echo "check_unit_no_sockets: $binary is not an executable"
    exit 2
fi

trace=$(mktemp)
trap 'rm -f -- "$trace"' EXIT

suite=0
# Filter in the kernel: ordinary -e filtering still stops at every unrelated
# syscall under ptrace. That multiplied the 1000-client Debug case's cost
# under CI load. -f retains fork/exec tracing, including child socket openers.
strace --seccomp-bpf -f -qq -s 256 -e trace=%network,execve -o "$trace" "$binary" "$@" || suite=$?

if [ ! -s "$trace" ]; then
    echo "check_unit_no_sockets: strace wrote no trace for $binary"
    exit 2
fi

# each line is "<pid> <call>"; a worker's execve names its case as its last argument.
# awk exits 1 on a network call, 3 when no case worker ran, 0 otherwise.
verdict=0
awk '
    $2 ~ /^execve\(/ {
        line = $0
        sub( /^[0-9]+ +execve\("[^"]*", \[/, "", line )
        sub( /\].*$/, "", line )
        n = split( line, args, /", "/ )
        name = args[n]
        gsub( /"/, "", name )
        cases[$1] = ( n > 1 ) ? name : "(the suite runner)"
        if ( n > 1 ) workers++
        next
    }
    $2 ~ /^[a-z_0-9]+\(/ && $2 !~ /^execve\(/ {
        count++
        if ( count == 1 ) {
            call = $0
            sub( /^[0-9]+ +/, "", call )
            opener = ( $1 in cases ) ? cases[$1] : ( "pid " $1 )
            first = opener ": " call
        }
    }
    END {
        if ( count > 0 ) {
            printf "FAIL check_unit_no_sockets: %d network call(s); first caller %s\n", count, first
            exit 1
        }
        if ( workers == 0 ) {
            print "FAIL check_unit_no_sockets: 0 cases ran (no case worker was started): nothing was checked"
            exit 3
        }
        printf "ok check_unit_no_sockets: no network system call in the unit suite (%d case workers)\n", workers
    }
' "$trace" || verdict=$?

if [ "$verdict" -eq 1 ]; then
    exit 1
fi
if [ "$suite" -ne 0 ]; then
    echo "FAIL check_unit_no_sockets: $binary exited $suite"
    exit "$suite"
fi
if [ "$verdict" -ne 0 ]; then
    exit 1
fi
