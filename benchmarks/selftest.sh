#!/usr/bin/env bash
#
# The bench harnesses' failure states, driven (#98 item 5).
#
#   bash benchmarks/selftest.sh          (or: make -C benchmarks selftest)
#
# #98's fifth finding was that the harnesses could not fail loudly: a GSO-less
# host read pps=0 instead of aborting, a full io_uring submission queue
# segfaulted instead of saying so, and every receiver treated any errno as an
# empty queue. Those are fixed. This script is what stops the fix from being a
# claim: it FORCES each abort path on a healthy host and fails unless the
# harness goes red with a message naming the call.
#
# Run it on any new bench host before the campaign. It takes seconds.
#
# The fault paths live in bench_common.h and are shared by every harness, so
# the legs below exercise the same code the Linux-only harnesses use even
# where those harnesses cannot be built. Legs for harnesses this host cannot
# build are reported as SKIPPED with the reason -- never silently dropped,
# because a selftest that quietly runs nothing passes on every machine.

set -u

cd "$( dirname "${BASH_SOURCE[0]}" )" || exit 1

failures=0
skipped=0

# Every harness that calls bench_fault_init announces synthetic results,
# so every one of them owes a refusal leg proving it can fail. The
# coverage check at the bottom fails if a BUILT harness got neither a leg nor
# a skip: an untested abort path is how #98 item 5 happened the first time.

# Derived from the source, not typed. A hardcoded list only covers the
# harnesses someone remembered, and a new one added later passes this script
# green while announcing synthetic results and then
# printing a rate. Anything that calls bench_fault_init owes a leg, so the
# list of things that call bench_fault_init IS the list.

ALL_HARNESSES=$( grep -l 'bench_fault_init' ./*.c 2>/dev/null | sed 's|^\./||; s|\.c$||' | tr '\n' ' ' )
if [ -z "$ALL_HARNESSES" ]; then
    echo "bench-selftest: found no harness calling bench_fault_init -- this script is looking in the wrong place" >&2
    exit 1
fi
COVERED=""

fail()
{
    echo "FAIL: $*" >&2
    failures=$(( failures + 1 ))
}

WORK="$( mktemp -d "${TMPDIR:-/tmp}/bench-selftest.XXXXXX" )"
trap 'rm -rf "$WORK"' EXIT

# Ports are derived from the pid rather than fixed. Two runs of this script
# overlapping -- back to back in a suite, or side by side on a busy CI box --
# otherwise reuse the same numbers, and a sender from the earlier run reaches
# a receiver in the later one. That does not produce a false PASS, but it
# produces a false FAIL: the receiver finds real data, takes the recvfrom
# path instead of the idle path, and aborts naming a different call than the
# leg expected. A selftest that cries wolf gets ignored, which is the same
# end state as one that never fires.

PORT_BASE=${BENCH_SELFTEST_PORT:-$(( 20000 + ( $$ % 20000 ) ))}

# next_port assigns to PORT rather than echoing. An earlier version echoed,
# and every caller read it as p=$( port ) -- a SUBSHELL, so the increment
# never reached the parent and all thirteen legs plus the healthy pair shared
# one port. The pid-derived base masked that for two separate runs and did
# nothing for two runs given the same BENCH_SELFTEST_PORT, which reproduced
# the false FAIL every time, control leg included.

# and each candidate is PROBED before use, not merely assumed free. A rolling
# base makes two runs unlikely to collide; probing makes a collision take a
# race rather than a coincidence. Note the words: this narrows the window
# between the probe and the harness's own bind, it does not close it, and a
# cold reader saw one failing pair in 19 attempts on a loaded machine after
# 16 clean ones. Both parts matter, because the symptom is not a red leg that
# names a port -- it is a leg aborting on the wrong call, or the healthy
# control going red, neither of which points at the environment.

next_port()
{
    local tries=0
    while [ "$tries" -lt 200 ]; do
        PORT_BASE=$(( PORT_BASE + 1 ))
        tries=$(( tries + 1 ))
        if [ -n "$PORT_PROBE" ]; then
            # 0.0.0.0, because that is what the harnesses bind. Probing
            # 127.0.0.1 would pass on a port already held on another
            # interface and hand back a number the harness then fails on.
            if "$PORT_PROBE" -c "
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
try:
    s.bind(('0.0.0.0', $PORT_BASE))
except OSError:
    sys.exit(1)
s.close()
" 2>/dev/null; then
                PORT=$PORT_BASE
                return
            fi
            continue
        fi
        PORT=$PORT_BASE                     # no probe available: the base alone
        return
    done
    echo "bench-selftest: found no free UDP port in 200 tries from $PORT_BASE" >&2
    exit 1
}

PORT_PROBE=$( command -v python3 || true )

# A leg: run one side of one harness with one fault armed, and require a
# nonzero exit carrying "BENCH ABORTED" and the named call.
#
#   leg <harness> <role> <fault> <expected text> <description>

leg()
{
    local harness="$1" role="$2" fault="$3" expect="$4" description="$5"
    local out="$WORK/${harness}_${role}_${fault}.txt"
    next_port
    local p="$PORT"

    if [ ! -x "./$harness" ]; then
        echo "SKIP  $harness $fault -- not built on this host ($description)"
        skipped=$(( skipped + 1 ))
        COVERED="$COVERED $harness"
        return
    fi
    COVERED="$COVERED $harness"

    # A callback-driven receiver has no drain loop to enter: it only reaches
    # the shared rule when a datagram completion arrives, so its receive leg
    # needs a sender. Everything else reaches it on its first empty drain.

    if [ "$role" = "recv-pair" ]; then
        # no external timeout: smoke_pair.sh is self-bounding (3 s warm-up,
        # one 5 s window) and `timeout` is not a POSIX tool -- macOS has no
        # such binary, and depending on one would make this leg pass by
        # vanishing on exactly the platform bench_nw exists for
        BENCH_FAULT="$fault" ./smoke_pair.sh "$harness" "$p" > "$out" 2>&1
    elif [ "$role" = "recv" ]; then
        BENCH_FAULT="$fault" ./"$harness" recv "$p" 1 1 1 > "$out" 2>&1
    else
        BENCH_FAULT="$fault" ./"$harness" send 127.0.0.1 "$p" 3 > "$out" 2>&1
    fi
    local status=$?

    if [ "$status" -eq 0 ]; then
        fail "$harness $role with BENCH_FAULT=$fault exited 0 -- $description did not abort, so the harness can still report numbers a broken host produced"
        return
    fi
    if ! grep -q 'BENCH ABORTED' "$out"; then
        fail "$harness $role with BENCH_FAULT=$fault exited $status but never printed BENCH ABORTED"
        tail -3 "$out" >&2
        return
    fi
    if ! grep -q "$expect" "$out"; then
        fail "$harness $role with BENCH_FAULT=$fault aborted but did not name '$expect'"
        tail -3 "$out" >&2
        return
    fi
    echo "leg   $harness $role $fault: red, exit $status"
    echo "        $( grep -m1 'BENCH ABORTED' "$out" )"
}

# ---------------------------------------------------------------------------
# receivers: an errno that is not an empty queue must abort, not read as empty.
# No sender is started -- the receiver's first drain comes back EAGAIN, which
# the fault turns into EBADF, which is the condition under test.

leg bench_portable recv recv-errno recvfrom  "a receive refused for a reason that is not an empty queue"
leg bench_kqueue   recv recv-errno kevent    "a kqueue readiness call refused"
leg bench_msgx     recv recv-errno recvmsg_x "a batch receive refused"
leg bench_gso      recv recv-errno recvmmsg  "a GRO batch receive refused"

# The io_uring receiver needs a provided-buffer ring before it can arm a
# multishot receive, and some kernels refuse IORING_REGISTER_PBUF_RING
# outright (EINVAL at every ring size). On such a host the receiver aborts at
# setup, before either fault below can be reached, so its two legs cannot
# prove anything there. One unfaulted probe decides: when it aborts naming
# io_uring_setup_buf_ring, both legs are SKIPPED with that line as the reason
# and counted; otherwise they run, and a host with a working buffer ring still
# has to show both faults aborting loudly.

uring_recv_refused=""
if [ -x ./bench_uring ]; then
    next_port
    env -u BENCH_FAULT ./bench_uring recv "$PORT" 1 1 1 > "$WORK/bench_uring_recv_probe.txt" 2>&1
    if grep -q 'BENCH ABORTED: io_uring_setup_buf_ring' "$WORK/bench_uring_recv_probe.txt"; then
        uring_recv_refused="$( grep -m1 'BENCH ABORTED' "$WORK/bench_uring_recv_probe.txt" )"
    fi
fi

uring_recv_leg()
{
    if [ -n "$uring_recv_refused" ]; then
        echo "SKIP  bench_uring recv $1 -- this host cannot run the receiver ($3); the unfaulted probe said: $uring_recv_refused"
        skipped=$(( skipped + 1 ))
        COVERED="$COVERED bench_uring"
        return
    fi
    leg bench_uring recv "$@"
}

uring_recv_leg recv-errno "io_uring multishot recv" "a multishot receive that failed, which used to be stepped over"
uring_recv_leg sqe-exhausted io_uring_get_sqe "a full submission queue, which used to be a NULL dereference"
leg bench_nw       recv-pair recv-errno nw_connection_receive_message "a Network.framework receive completion carrying an error, which used to be swallowed"

# ---------------------------------------------------------------------------
# senders: a persistent refusal that is not back pressure must abort rather
# than spin to a pps=0 report. No receiver is listening, which is fine: the
# fault forces the refusal regardless of what the socket would have done.

leg bench_portable send send-refused sendto    "a persistently refused send"
leg bench_kqueue   send send-refused sendto    "a persistently refused send"
leg bench_portable send send-short "sendto" "a short datagram send must refuse"
leg bench_portable send send-zero "sendto" "a zero datagram send must refuse"
leg bench_kqueue send send-short "sendto" "a short datagram send must refuse"
leg bench_kqueue send send-zero "sendto" "a zero datagram send must refuse"
leg bench_uring send send-short "io_uring send completion" "a short completion must refuse"
leg bench_uring send send-zero "io_uring send completion" "a zero completion must refuse"
leg bench_msgx     send send-refused sendmsg_x "a persistently refused batch send"
leg bench_gso      send send-refused UDP_SEGMENT "a GSO-less host, the pps=0 case #98 named"
leg bench_uring    send send-refused io_uring "a refused submission, which used to be a silent continue"
leg bench_nw       send send-refused nw_connection_send "a send completion carrying an error, which used to be discarded"

# ---------------------------------------------------------------------------
# Partial failures must invalidate a sample even when each success resets the
# consecutive-refusal counter. The final health check is shared by every sender.
for partial_harness in bench_portable bench_kqueue bench_msgx bench_gso bench_uring bench_nw; do
    leg "$partial_harness" send send-partial "send health: refused=" "31 of every 32 sends refused"
done

# ---------------------------------------------------------------------------
# Positive short batch results must account for each unsent message. Zero
# results have no successful prefix and must also fail instead of earning credit.
leg bench_msgx send send-short "send health: refused=" "one accepted message from a full batch"
leg bench_gso  send send-short "send health: refused=" "one accepted segment from a full GSO write"
leg bench_msgx send send-zero sendmsg_x "zero accepted messages from a batch"
leg bench_gso  send send-zero UDP_SEGMENT "zero accepted bytes from a GSO write"

# Back-pressure remains legal, but failed completions earn no sent credit.
# The plant accepts exactly one of every 32 completed sends.
if [ -x ./bench_nw ]; then
    next_port
    BENCH_FAULT=send-backpressure ./bench_nw send 127.0.0.1 "$PORT" 1 > "$WORK/nw-backpressure.txt" 2>&1
    status=$?
    if [ "$status" -ne 0 ]; then
        fail "nw back-pressure control failed with exit $status"
    elif python3 - "$WORK/nw-backpressure.txt" <<'PYCONTROL'
import pathlib, sys
lines = pathlib.Path(sys.argv[1]).read_text().splitlines()
rows = [line for line in lines if line.startswith('SENDER ')]
if len(rows) != 1:
    sys.exit('missing sender count')
fields = dict(value.split('=', 1) for value in rows[0].split()[1:])
sent, refused, pressure = (int(fields[name]) for name in ('pkts', 'refused', 'back_pressure'))
if sent <= 0 or refused != 31 * sent or refused != pressure:
    sys.exit('failed completions were credited as sent packets')
PYCONTROL
    then
        echo "leg   bench_nw back-pressure completion accounting: green"
    else
        fail "nw back-pressure completion accounting credited refused packets"
    fi
else
    echo "SKIP  bench_nw back-pressure completion accounting -- not built on this host"
    skipped=$(( skipped + 1 ))
fi

# ---------------------------------------------------------------------------
# and the control on the control: with NO fault armed, a short healthy pair
# must complete and report a window. A selftest whose legs all abort because
# the harness aborts unconditionally would otherwise pass.

if [ -x ./bench_portable ]; then
    next_port
    p="$PORT"
    ./smoke_pair.sh bench_portable "$p" > "$WORK/healthy.txt" 2>&1
    status=$?
    if [ "$status" -ne 0 ]; then
        fail "a healthy bench_portable pair exited $status -- the harnesses now abort when they should not"
        tail -5 "$WORK/healthy.txt" >&2
    elif ! grep -q '^WINDOW 0 ' "$WORK/healthy.txt"; then
        fail "a healthy bench_portable pair produced no WINDOW line"
        tail -5 "$WORK/healthy.txt" >&2
    elif grep -q 'BENCH ABORTED' "$WORK/healthy.txt"; then
        fail "a healthy bench_portable pair printed BENCH ABORTED"
    else
        echo "leg   bench_portable healthy pair: green"
        grep -E '^WINDOW 0' "$WORK/healthy.txt" | sed 's/^/        /'
    fi
else
    echo "SKIP  bench_portable healthy pair -- not built on this host"
    skipped=$(( skipped + 1 ))
fi

# ---------------------------------------------------------------------------
# The stop at the receiver's last window (PROTOCOL.md, pair_common.sh). Three
# legs, and they prove different things:
#
#   test_send_health.c   the classification alone, deterministically: an
#                        ECONNREFUSED before the stop fails the sample, any
#                        number of them after it do not.
#   in-window plant      end to end through smoke_pair.sh: one ECONNREFUSED
#                        inside the measured window must still fail the pair.
#   connected pairs      end to end on the harnesses whose connected sockets
#                        see the receiver's closed port as ECONNREFUSED
#                        (bench_uring, bench_nw). A healthy pair must be green;
#                        before the stop, every one of them failed here.

if ! "${CC:-cc}" -O2 -Wall -Wextra -Werror -ffp-contract=off ./test_send_health.c -o "$WORK/test_send_health" > "$WORK/test_send_health_build.txt" 2>&1; then
    fail "test_send_health compile failed"
    cat "$WORK/test_send_health_build.txt" >&2
elif ! "$WORK/test_send_health" > "$WORK/test_send_health.txt" 2>&1; then
    fail "test_send_health failed"
    cat "$WORK/test_send_health.txt" >&2
else
    echo "leg   send health around the stop: green"
fi

leg bench_portable recv-pair send-connrefused-window "send health: refused=1 " "one ECONNREFUSED inside the measured window"

connected_pair()
{
    local harness="$1"
    if [ ! -x "./$harness" ]; then
        echo "SKIP  $harness healthy connected pair -- not built on this host"
        skipped=$(( skipped + 1 ))
        return
    fi
    if [ "$harness" = bench_uring ] && [ -n "$uring_recv_refused" ]; then
        echo "SKIP  $harness healthy connected pair -- this host cannot run the receiver; the unfaulted probe said: $uring_recv_refused"
        skipped=$(( skipped + 1 ))
        return
    fi
    next_port
    env -u BENCH_FAULT ./smoke_pair.sh "$harness" "$PORT" > "$WORK/${harness}_connected.txt" 2>&1
    local status=$?
    if [ "$status" -ne 0 ]; then
        fail "a healthy $harness pair exited $status -- the sender was not stopped before the receiver closed, or a send was refused in the window"
        tail -5 "$WORK/${harness}_connected.txt" >&2
    elif ! grep -q '^WINDOW 0 ' "$WORK/${harness}_connected.txt"; then
        fail "a healthy $harness pair produced no WINDOW line"
    else
        echo "leg   $harness healthy connected pair: green"
        grep -E '^(WINDOW 0|SENDER)' "$WORK/${harness}_connected.txt" | sed 's/^/        /'
    fi
}

connected_pair bench_uring
connected_pair bench_nw

# ---------------------------------------------------------------------------
# driver_dispatch.cpp + driver_dispatch_fixture.cpp (transport_fill.md):
# compile and run the pair in $WORK (never the checkout root) and fail if
# either step fails.

CXX_BIN="${CXX:-c++}"
if ! "$CXX_BIN" -O3 -DNDEBUG -std=c++11 -ffp-contract=off -Wall -Wextra -Wpedantic -Werror -I ../sdk \
    ./driver_dispatch.cpp ./driver_dispatch_fixture.cpp -o "$WORK/dispatch" > "$WORK/dispatch_build.txt" 2>&1; then
    fail "driver_dispatch compile failed"
    cat "$WORK/dispatch_build.txt" >&2
elif ! "$WORK/dispatch" > "$WORK/dispatch_run.txt" 2>&1; then
    fail "driver_dispatch run failed"
    tail -5 "$WORK/dispatch_run.txt" >&2
elif ! grep -q '^sample=0 direct_ns=' "$WORK/dispatch_run.txt"; then
    fail "driver_dispatch produced no sample=0 direct_ns= output"
    tail -5 "$WORK/dispatch_run.txt" >&2
else
    echo "leg   driver_dispatch: green"
    head -1 "$WORK/dispatch_run.txt" | sed 's/^/        /'
fi

# ---------------------------------------------------------------------------

# coverage: a built harness with no leg at all is the failure this script
# exists to make impossible, so it is checked rather than left to the reader
# noticing an absence.

for h in $ALL_HARNESSES; do
    case " $COVERED " in
        *" $h "*) ;;
        *) if [ -x "./$h" ]; then
               fail "$h is built on this host and has NO leg in this script -- it calls bench_fault_init and announces that a fault must abort it, and nothing here checks that it can"
           else
               echo "SKIP  $h -- not built on this host, and no leg claims otherwise"
               skipped=$(( skipped + 1 ))
           fi ;;
    esac
done

if [ "$skipped" -ne 0 ]; then
    echo "bench-selftest: $skipped leg(s) skipped for harnesses this host cannot build or run"
fi

if [ "$failures" -ne 0 ]; then
    echo "bench-selftest: $failures leg(s) failed" >&2
    exit 1
fi

exit 0
