#!/bin/sh
# rocketnet transport bench campaign runner (#81, landed by #98 item 7).
#
# One sitting, start to finish, reproducible from the repo instead of from a
# shell history on the box: preflight the host, run the harness-honesty plants,
# then the full 5 x 60 s matrix, writing everything to a results directory that
# names the machine and the protocol revision.
#
#   ./run_all.sh <results-dir> [recv_core] [send_core]
#
# Cores are optional; on a host with taskset they pin the pair exactly as
# run_pair.sh does, and the report records whether they were used. Every number
# this produces belongs to the machine it produced them on -- magnitude does not
# travel, ordering sometimes does, and the report says so in its own header.
#
# Every required control and result record is checked before it enters the
# summary. Timing qualifications below still apply to the raw latency columns.

set -e

RESULTS="${1:-results}"
RECV_CORE="$2"
SEND_CORE="$3"

mkdir -p "$RESULTS"
REPORT="$RESULTS/run.txt"

say()
{
    echo "$@" | tee -a "$REPORT"
}

: > "$REPORT"

say "rocketnet transport bench campaign"
say "date            $( date -u +%Y-%m-%dT%H:%M:%SZ )"
say "host            $( uname -srm ) $( hostname )"
say "repo revision   $( git -C .. rev-parse --short HEAD 2>/dev/null || echo 'not a git checkout' )"
say "cores           recv=${RECV_CORE:-unpinned} send=${SEND_CORE:-unpinned}"
say ""

# ---------------------------------------------------------------------------
# preflight
#
# The socket buffer ceilings are the one host setting that silently caps
# throughput. #81 measured on a box where net.core.{r,w}mem_max sat at the
# 212992 default, so a 4 MB request became an effective 425984 -- noted at the
# time, and repeated here because a production-class instance would want them
# raised BEFORE anything is trusted. This preflight does not change them: a
# bench runner that mutates the machine it is measuring is its own confound.

say "--- preflight"

if [ -r /proc/sys/net/core/rmem_max ]; then
    RMEM=$( cat /proc/sys/net/core/rmem_max )
    WMEM=$( cat /proc/sys/net/core/wmem_max )
    say "net.core.rmem_max   $RMEM"
    say "net.core.wmem_max   $WMEM"
    if [ "$RMEM" -le 212992 ] || [ "$WMEM" -le 212992 ]; then
        say ""
        say "WARNING: socket buffer ceilings are at or below the kernel default (212992)."
        say "         The harnesses request 4 MB and will be capped well under it. Raise"
        say "         them before trusting these numbers:"
        say "             sudo sysctl -w net.core.rmem_max=16777216 net.core.wmem_max=16777216"
        say "         Every number in this run is recorded WITH that caveat attached."
        say ""
    fi
else
    say "socket buffer ceilings  not readable on this platform (not Linux); the"
    say "                        per-socket effective sizes are reported by each"
    say "                        harness's own 'buffers:' line"
fi

if [ -x ./probe ]; then
    say "running ./probe -> $RESULTS/probe.txt"
    ./probe > "$RESULTS/probe.txt" 2>&1 || say "probe exited nonzero -- see $RESULTS/probe.txt"
elif [ -x ./probe_darwin ]; then
    say "running ./probe_darwin -> $RESULTS/probe.txt"
    ./probe_darwin > "$RESULTS/probe.txt" 2>&1 || say "probe exited nonzero -- see $RESULTS/probe.txt"
else
    say "no probe binary built -- the feature report is missing from this sitting"
fi
say ""

# which harnesses exist on this host. The control is mandatory: it is what
# makes two sittings comparable at all, and a campaign without it is a set of
# numbers with no shared scale.

HARNESSES=""
for h in bench_portable bench_gso bench_uring bench_msgx bench_kqueue bench_nw; do
    [ -x "./$h" ] && HARNESSES="$HARNESSES $h"
done

case " $HARNESSES " in
    *" bench_portable "*) ;;
    *) say "REFUSED: bench_portable is not built. It is the negative control and it runs"
       say "         in every sitting on every machine -- without it this run cannot be"
       say "         compared to any other, so it is not run at all."
       exit 1 ;;
esac

say "harnesses      $HARNESSES"
say ""

# ---------------------------------------------------------------------------
# harness honesty, before any measurement is taken
#
# The negative-control doctrine: before trusting a harness, break something it
# must see and confirm the numbers move. Two plants, and they catch different
# lies:
#
#   pace=<pps>   the sender injects a KNOWN rate. The receiver must read that
#                rate back. This is the one the rate instrument cannot fake --
#                a flat-out sender saturates and pps does not move whatever the
#                harness does wrong.
#   pace=half    a second known rate checks that the rate instrument follows
#                a changed input; skipping unpaced batches has no fixed ratio.
#
# Each window must report the requested rate within 10%, or the campaign stops.

PACE=100000

# Ports derive from the pid rather than being fixed, the #107 class: two
# campaigns on one box -- or a campaign beside a smoke run -- otherwise bind
# the same numbers, and the loser reports a rate of zero that reads like a
# slow transport rather than like a busy port.

PORT_BASE=${BENCH_PORT_BASE:-$(( 20000 + ( $$ % 20000 ) ))}
say "port base       $PORT_BASE (derived from pid; override with BENCH_PORT_BASE)"
say ""

say "--- harness honesty plants"
for h in $HARNESSES; do
    for rate in "$PACE" "$(( PACE / 2 ))"; do
        output="$RESULTS/honesty_${h}_${rate}.txt"
        say "$h: pace control at $rate pps"
        if ! ./smoke_pair.sh "$h" $(( PORT_BASE + 1 )) "$RECV_CORE" "$SEND_CORE" "pace=$rate" > "$output" 2>&1; then
            say "REFUSED: $h control process failed; see $output"
            exit 1
        fi
        if ! summary=$(python3 ./check_windows.py "$output" --windows 1 --expected-rate "$rate"); then
            say "REFUSED: $h pace control is invalid; see $output"
            exit 1
        fi
        say "$summary"
    done
done
say ""

run_matrix_pair()
{
    if [ "$(uname -s)" = Darwin ]; then
        ./run_pair_darwin.sh "$1" "$2" "${3:-}"
    else
        ./run_pair.sh "$1" "$2" "$RECV_CORE" "$SEND_CORE" "${3:-}"
    fi
}

# ---------------------------------------------------------------------------
# the matrix
#
# 30 s warm-up discarded, five 60 s windows, per the #81 discipline. The median
# with min/max spread is the result; the spread is the variance floor, and two
# candidates inside each other's spread are TIED and get said so.

say "--- the matrix (30 s warm-up, 5 x 60 s windows per harness)"
PORT=$(( PORT_BASE + 100 ))
for h in $HARNESSES; do
    say "$h -> $RESULTS/$h.txt"
    if ! run_matrix_pair "$h" "$PORT" > "$RESULTS/$h.txt" 2>&1; then
        say "REFUSED: $h matrix process failed; see $RESULTS/$h.txt"
        exit 1
    fi
    if ! summary=$(python3 ./check_windows.py "$RESULTS/$h.txt" --windows 5); then
        say "REFUSED: $h matrix records are invalid"
        exit 1
    fi
    say "$summary"
    PORT=$(( PORT + 1 ))
    say ""
done

# io_uring's zero-copy variant is a separate row, not a separate harness
if [ -x ./bench_uring ]; then
    say "bench_uring zc -> $RESULTS/bench_uring_zc.txt"
    if ! run_matrix_pair bench_uring "$PORT" zc > "$RESULTS/bench_uring_zc.txt" 2>&1; then
        say "REFUSED: bench_uring zc matrix process failed"
        exit 1
    fi
    if ! summary=$(python3 ./check_windows.py "$RESULTS/bench_uring_zc.txt" --windows 5); then
        say "REFUSED: bench_uring zc matrix records are invalid"
        exit 1
    fi
    say "$summary"
    say ""
fi

# ---------------------------------------------------------------------------

say "--- what these files do and do not say"
say ""
say "pps is a count over a wall-clock window. It is immune to where the harness"
say "puts its timer and it is the column to cite."
say ""
say "p50_ns and p99_ns are BATCH AVERAGES, not per-packet tails: a batch's wall"
say "time is divided by the packets in it and that one quotient is credited to"
say "every packet in the batch, so a real tail is smeared and p99 UNDERSTATES it."
say "Any citation of those columns must carry that sentence. 'timed=' and"
say "'timed_pkts=' say how many batches and packets fed them: a window covering"
say "under 25% of its packets prints NS COLUMNS UNREPRESENTATIVE with the exact"
say "fraction, and one covering none prints NS COLUMNS VOID. Do not cite the ns"
say "columns of any window carrying either line -- and note the floor is a"
say "threshold on QUANTITY while the bias is CATEGORICAL, so a window just above"
say "it is unwarned but still congestion-selected. See PROTOCOL.md."
say ""
say "The syscall columns are the harness counting its own calls. Verify them"
say "externally before publishing -- see PROTOCOL.md, section 'strace'."
say ""
say "Magnitude belongs to this machine. Ordering sometimes travels; magnitude"
say "never does. Loopback is not a NIC."
say ""
say "campaign complete -- $RESULTS"
