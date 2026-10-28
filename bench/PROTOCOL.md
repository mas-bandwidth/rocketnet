# The bench protocol

The rules a sitting is run under, landed into the repo by #98 item 7 so the
c6gn/GameLift rerun inherits them instead of reconstructing them from a shell
history on a box nobody else can reach. Everything here is #81's plan and its
first sitting's practice, written down; nothing here is new policy.

## Before anything

**Raise the socket buffer ceilings.** The harnesses request 4 MB per socket
and the kernel silently caps that at `net.core.{r,w}mem_max`. The #81 sitting
ran on a box where both sat at the 212992 default, giving an effective 425984,
which was recorded as a caveat on every number. On any instance whose numbers
are meant to stand:

    sudo sysctl -w net.core.rmem_max=16777216 net.core.wmem_max=16777216

`run_all.sh` reads them and warns; it does not set them. A runner that mutates
the machine it is measuring is its own confound.

**Run the probe.** `./probe` on Linux, `./probe_darwin` on macOS. It is a
feature report, not a benchmark, and on a host we have never had a shell on
before it is the single highest-information run available — it can decide the
production transport before any measurement does. Keep its output with the
results.

## The run

- 30 s warm-up, **discarded** — page-in, route caches, ring steady state.
- Five 60 s measured windows.
- Report the **median with min/max spread**. The spread is the variance floor.
  Two candidates inside each other's spread are **tied**, and the report says
  tied rather than picking one.
- The portable control (`bench_portable`) runs **in every sitting on every
  machine**. It is what makes two sittings comparable at all: if the control
  moves between sittings, the machine moved, not the code. `run_all.sh` refuses
  to run without it.
- Both sides at `nice 10`, pinned to isolated cores where the platform has an
  affinity API. macOS has none, so darwin runs are unpinned and the report says
  so.

## Harness honesty, before trusting any number

The doctrine, from #81 §3: *before trusting a harness, break something it must
see and confirm the numbers move. A bench that cannot detect a planted
regression has measured nothing.* Two plants, catching different lies:

**The pace plant.** `pace=<pps>` makes the sender inject a known rate; the
receiver must read that rate back. This is the one a rate instrument cannot
fake. A flat-out sender saturates, so a harness that miscounts still reports a
plausible-looking pps — under a pace plant it cannot.

    ./smoke_pair.sh bench_portable 9301 15 14 pace=100000
    # the WINDOW line's pps must come back at ~100000

**The half-pace plant.** A second pace at half the first checks that the rate
instrument follows a changed input; skipping unpaced batches has no fixed
ratio, so a receiver that reports a constant cannot pass both.

`run_all.sh` runs the pace plant at both rates for every built harness before
it takes a single measurement, and writes them to
`honesty_<harness>_<rate>.txt`.

## The fail-loud check

Every harness aborts rather than reporting a number when the host cannot do
what it is being asked to measure (#98 item 5): a receive refused for any
reason that is not an empty queue, a long unbroken run of sends refused for any
reason that is not back pressure, an io_uring submission queue with no free
entry. Before #98 a GSO-less host reported `pps=0` from the GSO harness, which
reads exactly like a very slow transport.

`BENCH_FAULT=<name>` forces each of those paths on a healthy host, and
`bench/selftest.sh` runs them and fails unless each one goes red. Run it on any
new bench host before the campaign — it takes seconds and it is the difference
between "the harness aborts loudly" being a claim and being a fact:

    make -C bench selftest

## strace: verifying the syscall columns externally

The harnesses count their own syscalls, which is exactly the number a harness
can be wrong about. Verify externally on a **separate short run**, never during
a measured window (strace's overhead is enormous and would corrupt every column
it shares a run with):

    ./bench_portable recv 9501 3 5 1 &
    strace -c -f -p $!            # then start a sender against 9501

Read two things off it. First, the **shape**: the data path should be
essentially one syscall type — the #81 sitting read 99.7% `recvfrom` for
portable, 99.7% `recvmmsg` for GSO, 99.7% `io_uring_enter` for io_uring, and a
harness whose profile is not that shape is doing something its own description
does not mention. Second, the **counts**, against the harness's self-reported
`syscalls=` field over a comparable interval.

Two known discrepancies to expect rather than chase:

- **io_uring zero-copy overstates.** liburing peeks the completion queue before
  entering the kernel, so many `io_uring_wait_cqe` calls make no syscall at
  all; the harness counts every one as an enter. The ZC row's syscall column is
  an ordering, not a count (#98 item 4).
- **Sender counts are by construction.** The sender is stopped when the
  receiver's last window closes: the receiver signals it (SIGUSR1, through the
  pid the pair runner writes to `BENCH_SENDER_PIDFILE`) and holds its socket
  open for one more second, and the runner signals it again once the receiver
  has exited. The sender then validates the sends it made up to the stop,
  prints its `SENDER` line and exits, and the pair fails if it exits nonzero.
  A send refused anywhere in the measured windows therefore still fails the
  sample. A send refused after the stop is outside the sample: on a connected
  socket (io_uring, Network.framework) the receiver's closed port comes back as
  ECONNREFUSED, and those are reported as `SENDER late_refused=` and not
  counted. The send-side counts are one per packet for the portable control
  and one per batch for GSO and io_uring, by construction, and were confirmed
  in short runs.

## Reading the columns

**`pps`** is a count over a wall-clock window. It is timer-placement-immune and
it is the column to cite.

**`p50_ns` / `p99_ns` are batch averages, not per-packet tails.** A batch's wall
time is divided by the packets in it and that single quotient is credited to
every packet in the batch, so one slow packet among thirty-one fast ones
reports thirty-two medium packets. The tail is smeared and **p99 understates a
real per-packet tail**. Any table quoting these columns restates that in one
sentence (#98 item 1).

**`timed=`** says how many batches in the window fed the ns columns, and
**`timed_pkts=`** how many packets those batches carried. Batches that ended on
an empty syscall are excluded, because that empty syscall sits inside the timed
region and inflates the quotient for every packet the batch did carry (#98
item 8).

The exclusion can leave the columns resting on very little, and what survives
is not a neutral sample: the batches that fill completely are the congested
ones. So the check is a SHARE, not a nonzero test. A window whose `timed_pkts`
covers less than 25% of its packets prints `NS COLUMNS UNREPRESENTATIVE` with
the exact fraction, and a window with `timed=0` prints `NS COLUMNS VOID`. Do
not cite the ns columns of a window carrying either line.

Two things that 25% floor does not fix, stated so nobody reads more into a
clean window than is there. It is a threshold on QUANTITY while the bias is
CATEGORICAL: the inclusion rule is "the batch filled" at every share, so a
window at 26% coverage prints without a warning and is still congestion-
selected. And on a saturated loopback the shortfall can be extreme --
`bench_portable` on the test machine covered 0.04% -- so the absence of a warning
is worth checking rather than assuming.

**`empty=`** is the drain's idle probes. At saturation a receiver spends real
time on them; they are not per-packet work and do not belong in a per-packet
cost.

**`back_pressure=`** on the sender line counts sends refused with ENOBUFS or
EAGAIN — the socket buffer saying slow down. Nonzero is normal at saturation.
It is reported separately from the abort path precisely so that "the kernel is
full" and "the kernel refuses this call" never look alike.

## What travels between machines, and what does not

**Magnitude belongs to the machine.** A number measured on the test box orders
candidates on the test box and nowhere else. Every published number carries
machine, kernel, toolchain and protocol revision.

**Ordering sometimes travels, and must be said to be a hypothesis when it does.**
The #81 sitting's ordering was measured over **loopback** — MTU 65536, one
4-tuple pair, no NIC or IRQ path, GRO coalescing at its friendliest. That is a
different machine from a c6gn's ENA path in every respect that the candidates
differ in. The loopback ordering is the reason to run the NIC sitting, not a
substitute for it.

## Cross-references

- #81 — the measurement plan, the matrix, and the first sitting's numbers.
- #95 — the instruments' cold review.
- #98 — the eight findings this file and the #98 commits close.
- #100 — the darwin harnesses and the `poll=` receiver shape.
