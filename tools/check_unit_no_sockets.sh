#!/usr/bin/env bash
# The unit suite opens no socket, starts no thread and sleeps no real time
# (the owner's rule: unit tests use no real time, no sockets, no network and no
# outside services). Runs the unit binary under
# strace --seccomp-bpf -f -e trace=socket,execve,clone,clone3,nanosleep,clock_nanosleep
# and exits nonzero, naming the first offender (the case that ran it, read from
# the worker's execve, and the call), when any process of the suite calls
# socket(), starts a thread, or sleeps; or when a case's process starts a
# process. The parallel runner's own machinery is allowed: the suite runner
# (the first process) may spawn workers and sleep while it polls them, and an
# exec'd worker (--run <case>) may fork the child that runs its case. A process
# nobody exec'd (a case's forked child) may start nothing. execve is traced to
# name the case and to tell the runner's processes from a case's. A failing
# suite fails the check too, and the suite's own output prints, so on Linux CI
# this run is the plain unit run as well.
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
strace --seccomp-bpf -f -qq -s 256 -e trace=socket,execve,clone,clone3,nanosleep,clock_nanosleep -o "$trace" "$binary" || suite=$?

if [ ! -s "$trace" ]; then
    echo "check_unit_no_sockets: strace wrote no trace for $binary"
    exit 2
fi

# each line is "<pid> <call>"; a worker's execve names its case as its last
# argument, and a clone's result names the new process or thread, which takes
# its parent's case until it exec's its own
awk '
    function call_of( line ) { sub( /^[0-9]+ +/, "", line ); return line }
    NR == 1 { root = $1 }
    $2 ~ /^execve\(/ && $0 !~ /= -1 / {
        line = $0
        sub( /^[0-9]+ +execve\("[^"]*", \[/, "", line )
        sub( /\].*$/, "", line )
        n = split( line, args, /", "/ )
        name = args[n]
        gsub( /"/, "", name )
        cases[$1] = ( n > 1 ) ? name : "(the suite runner)"
        execd[$1] = 1
        next
    }
    $2 ~ /^(clone3?\(|<\.\.\. clone3? resumed>)/ && match( $0, /= [0-9]+$/ ) {
        parent[substr( $0, RSTART + 2 )] = $1
    }
    $2 ~ /^clone3?\(/ {
        events++
        epid[events] = $1
        ecall[events] = call_of( $0 )
        ekind[events] = ( $0 ~ /CLONE_THREAD/ ) ? "thread" : "process"
    }
    $2 ~ /^(clock_)?nanosleep\(/ {
        events++
        epid[events] = $1
        ecall[events] = call_of( $0 )
        ekind[events] = "sleep"
    }
    $2 ~ /^socket\(/ {
        events++
        epid[events] = $1
        ecall[events] = call_of( $0 )
        ekind[events] = "socket"
    }
    function case_of( pid,    hops ) {
        for ( hops = 0; hops < 64 && !( pid in cases ) && ( pid in parent ); hops++ )
            pid = parent[pid]
        return ( pid in cases ) ? cases[pid] : ( "pid " pid )
    }
    END {
        for ( i = 1; i <= events; i++ ) {
            pid = epid[i]
            kind = ekind[i]
            if ( kind == "sleep" && pid == root )
                continue
            if ( kind == "process" && ( pid in execd ) )
                continue
            count[kind]++
            if ( !( kind in first ) )
                first[kind] = case_of( pid ) ": " ecall[i]
        }
        failed = 0
        if ( count["socket"] > 0 ) {
            printf "FAIL check_unit_no_sockets: %d socket() call(s); first opener %s\n", count["socket"], first["socket"]
            failed = 1
        }
        if ( count["thread"] > 0 ) {
            printf "FAIL check_unit_no_sockets: %d thread start(s); first %s\n", count["thread"], first["thread"]
            failed = 1
        }
        if ( count["process"] > 0 ) {
            printf "FAIL check_unit_no_sockets: %d process start(s) by a case; first %s\n", count["process"], first["process"]
            failed = 1
        }
        if ( count["sleep"] > 0 ) {
            printf "FAIL check_unit_no_sockets: %d real sleep(s) outside the suite runner; first %s\n", count["sleep"], first["sleep"]
            failed = 1
        }
        if ( failed )
            exit 1
        print "ok check_unit_no_sockets: no socket(), thread start, sleep or case process start in the unit suite"
    }
' "$trace" || exit 1

if [ "$suite" -ne 0 ]; then
    echo "FAIL check_unit_no_sockets: $binary exited $suite"
    exit "$suite"
fi
