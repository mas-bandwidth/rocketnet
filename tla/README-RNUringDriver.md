# RNUringDriver: the io_uring server driver

`RNUringDriver.tla` models the io_uring server driver
(`sdk/rocketnet_server_uring_linux.cpp`) **before the code exists**. It
follows the driver's design. Once the driver
lands, it cites this module, and the `io-uring-driver` row in
`COVERAGE.tsv` moves from `owed` to `current`.

## The abstraction

The driver owns one io_uring, and only the main thread uses it. The model
keeps the three state machines of the design and abstracts away the
contents:

- **Send slots.** Each slot is in one of four states: `Free`, `Reserved`,
  `Finished` or `Kernel`. A `bytes` flag says whether finish recorded bytes
  or abort zeroed them. `idx` is the reservation index. `kown` is the set of
  slots whose frame and msghdr the kernel owns, from the moment of submit to
  the moment of the send CQE. `pending` is the set of slots this
  `send_packets` submitted whose CQE has not been reaped yet. `sending` is
  true while the main thread is inside `send_packets`.
- **Receive.** Each ring buffer is in one of three states: `Ring`, `Filled`
  or `Copied`. `bufdg` is the datagram last filled into each buffer.
  `copied` is the set of datagrams copied out into a bank. The banks are
  `fill` and `exposed`, each a set of datagram ids. `snap` is the exposed
  bank as the last Swap left it, and `gen` is the exposed generation.
  `armed` is true while the multishot RECVMSG is active in the kernel.
- **Completions.** `cq` is the set of pending CQEs: a send CQE per slot, a
  receive CQE per buffer, `Term` (a multishot CQE without F_MORE) and
  `Cancel` (the CQE of the cancel SQE). A set is enough because each CQE
  is unique while it is pending.
- **Phase:** `Running`, `Destroying` or `Freed`.

Two constants act as rule switches and carry the reversed witnesses:
`ResetWaitsForReap` and `ReapIntoFill`. Both are `TRUE` in the rule.

## Each action against THE DESIGN

| Action | Design sentence it models |
|---|---|
| `Reserve` | start_packet reserves the next slot with one atomic fetch_add; it returns NULL, with no step, when the staging is full |
| `Finish(s)` | finish records the bytes |
| `Abort(s)` | abort zeroes them; send_packets then skips the slot |
| `Submit` | send_packets, with the fill threads joined (no slot still `Reserved`), adds one SENDMSG SQE for each slot with bytes > 0 and submits; from then on the kernel owns those frames |
| `KernelComplete(s)` | the kernel finishes the send (any res, where a negative one counts SEND_DROPPED) and posts the send CQE |
| `KernelFill(b)` | the armed multishot RECVMSG fills a provided ring buffer and posts a receive CQE |
| `KernelStop` | a CQE without IORING_CQE_F_MORE ends the multishot (ENOBUFS among the causes) |
| `Reap(c)` | the main thread reaps one CQE, inside send_packets or outside it. A send CQE frees nothing by itself; it only removes the slot from `pending`. A receive CQE is copied into the **fill** bank. `Term` re-arms the multishot while the driver is running and never during destroy |
| `ReturnBuffer(b)` | after the copy, the buffer goes back to the ring |
| `ResetIndex` | send_packets resets the reservation index to 0 only when no slot is `Kernel` owned and every send it submitted has its CQE reaped; the call returns |
| `Swap` | receive_packets: the old fill bank becomes the exposed bank and the new fill bank is emptied |
| `TransportReads` | the transport reads the views of the exposed bank. A read changes no driver state, so here it is a stutter; what the read relies on is ExposedStable |
| `Destroy` | cancels every multishot receive: the cancel posts its own CQE, and an armed multishot ends with a `Term` |
| `Free` | destroy's drain ends: once no slot is kernel owned, the cq is empty and the multishot is disarmed, slots, banks and the ring are freed |
| `Done` | the driver is gone; the stutter at the end of a run |

## Properties

Invariants:

- `TypeOK`
- `NoReuseWhileKernelOwned`: a slot held by a fill thread (`Reserved` or
  `Finished`) is never in `kown`, so Reserve never returns a slot the
  kernel owns.
- `ExposedStable`: `exposed = snap`, so the exposed bank never changes
  between two Swaps.
- `BufferReturnedAfterCopy`: a buffer is back in `Ring` only after its
  datagram is in `copied`.
- `FreedOnlyWhenDrained`: in `Freed` there is no kernel-owned slot, no
  `Filled` buffer, and the cq is empty.

Liveness, under `SpecFair`:

- `SubmittedEventuallyFree`: every `Kernel` slot is eventually `Free`.
- `DestroyEventuallyFrees`: `Destroying ~> Freed`.

`SpecFair` gives weak fairness to the following:

- **`KernelComplete(s)`, per slot.** The kernel completes every send.
- **`Reap(c)`, per CQE.** A CQ is FIFO, so a pending CQE is never starved
  by later ones. With a single disjunctive fairness on Reap, TLC could pick
  `Term` forever while a send CQE waits.
- **`ReturnBuffer(b)`.**
- **`ResetIndex`.** This is the end of send_packets, which does not return
  without it.
- **`Free`.** This is destroy's drain.

The brief names fairness of KernelComplete, Reap and the drain. ResetIndex
and ReturnBuffer are steps of send_packets and of the reap, so they get
fairness too.

## What the model leaves out

- Byte layout, the msghdr and iovec contents, and address conversion.
- The GSO-style SO_REUSEPORT socket group. The N queues are folded into
  one multishot receive. Each queue arms and re-arms on its own in the same
  way, and the banks are shared.
- SQ-full handling and partial submits. The ENOBUFS counter and the
  SEND_DROPPED counter are counts only, so they are not modelled.
- How the fill threads share the work. Their concurrency reaches the model
  only through the single fetch_add.

## The small instance and the runs

`MCRNUringDriver.tla` extends the module with no overrides. The constants
are `Slots = 2`, `Buffers = 2` and `MaxSteps = 2` (two datagrams, two bank
swaps). The three cases form group `rnuringdriver` in `CASES.tsv`.

To run them from the rocketnet checkout, with `$job`, `$jar` and `$java`
as in `tla/README.md`:

```sh
for i in 0 1 2; do
  "$job/tlacheck" run --root . --jar "$jar" --java "$java" \
    --group rnuringdriver --shards 3 --shard "$i" --workers 2 --timeout 110s \
    --dir "$job/rnuringdriver-$i"
done
"$job/tlacheck" merge --root . --keep tla/RUNS.tsv \
  --out "$job/RUNS.tsv" "$job"/rnuringdriver-*/RUNS.tsv
```

Outcomes measured on Linux (linux-amd64, Java 21.0.12.1, the pinned JAR):

| Case | Expected | Exit | Generated | Distinct | Seconds |
|---|---|---:|---:|---:|---:|
| `MCRNUringDriver.cfg` | pass (5 invariants, 2 properties) | 0 | 119869 | 31373 | 67.5 |
| `MCRNUringDriverBrokenResetBeforeReap.cfg` | `NoReuseWhileKernelOwned` | 12 | 1007 | 459 | 6.6 |
| `MCRNUringDriverBrokenOneBank.cfg` | `ExposedStable` | 12 | 27 | 22 | 6.1 |

`MaxSteps = 3` also passed when run by hand with raw TLC: 194972 distinct
states in 2 min 51 s, over the 110 s record budget. That run is not a
record. Under the budget rule, the committed instance is `MaxSteps = 2`.

## Counterexamples, checked by hand against THE DESIGN

**BrokenResetBeforeReap** (`ResetWaitsForReap = FALSE`): ResetIndex may run
right after Submit. TLC printed six states:

1. Init.
2. `Reserve`: slot 1 is `Reserved`, idx = 1.
3. `Finish(1)`: slot 1 is `Finished` with bytes.
4. `Submit`: slot 1 is `Kernel`, kown = {1}, pending = {1}.
5. `ResetIndex`: idx = 0 and every slot is `Free`, but kown and pending
   are still {1}.
6. `Reserve`: slot 1 is `Reserved` while 1 is still in kown.

By hand, against the design: send_packets submitted slot 1's SENDMSG, and
until its CQE the kernel owns slot 1's frame and msghdr. A reset before the
reap lets the next start_packet's fetch_add return slot 1, and a fill
thread then writes the frame the kernel is still sending. That is the
corruption the design rules out with "only then does it reset the
reservation index to 0". The trace is a real defect of the broken rule, not
a model artifact.

**BrokenOneBank** (`ReapIntoFill = FALSE`): Reap copies a receive CQE into
the exposed bank. TLC printed three states:

1. Init.
2. `KernelFill(1)`: buffer 1 is `Filled` with datagram 1, and its receive
   CQE is in cq.
3. `Reap`: buffer 1 is `Copied` and exposed = {1}, while snap = {} and
   gen = 0.

By hand, against the design: the transport is reading the views of the
exposed bank of generation 0, and no receive_packets has run. Under the
broken rule, a datagram reaped by the main thread lands in that bank, so
the bank the transport is reading changes under it. The design forbids
this with "Receive CQEs ... always go to the fill bank". This is a real
defect.

Each witness fails on exactly its declared invariant (exit 12), and the
control passes every invariant and both liveness properties.
