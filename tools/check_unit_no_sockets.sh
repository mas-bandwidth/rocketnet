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
# prints, so on Linux CI this run is the plain unit run as well.
#
# usage: tools/check_unit_no_sockets.sh [binary]   (default ./bin/test)

set -eu

binary=${1:-./bin/test}

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
strace --seccomp-bpf -f -qq -s 256 -e trace=%network,execve -o "$trace" "$binary" || suite=$?

if [ ! -s "$trace" ]; then
    echo "check_unit_no_sockets: strace wrote no trace for $binary"
    exit 2
fi

# each line is "<pid> <call>"; a worker's execve names its case as its last argument
awk '
    $2 ~ /^execve\(/ {
        line = $0
        sub( /^[0-9]+ +execve\("[^"]*", \[/, "", line )
        sub( /\].*$/, "", line )
        n = split( line, args, /", "/ )
        name = args[n]
        gsub( /"/, "", name )
        cases[$1] = ( n > 1 ) ? name : "(the suite runner)"
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
        print "ok check_unit_no_sockets: no network system call in the unit suite"
    }
' "$trace" || exit 1

if [ "$suite" -ne 0 ]; then
    echo "FAIL check_unit_no_sockets: $binary exited $suite"
    exit "$suite"
fi
