# RNUringDriver: the io_uring server driver

`RNUringDriver.tla` models the io_uring server driver
(`sdk/rocketnet_server_uring_linux.cpp`). The driver names the model's
actions in its comments, and the `io-uring-driver` row in `COVERAGE.tsv`
tracks the coverage. The code receives on one socket per queue, each with its
own multishot RECVMSG and provided buffer ring; the model abstracts those to
one multishot and one buffer ring.

## The abstraction

The driver owns one io_uring, and only the main thread uses it. The model
keeps the three state machines of the code and abstracts away the
contents:

- **Send slots.** Each slot is in one of four states: `Free`, `Reserved`,
  `Finished` or `Kernel`. A `bytes` flag says whether finish recorded bytes
  or abort zeroed them. `idx` is the reservation index. `kown` is the set of
  slots whose frame and msghdr the kernel owns, from the moment of submit to
  the moment of the send CQE. A send is a **run** of slots: one slot, or,
  with `UDP_SEGMENT`, consecutive slots sent as one segmented SENDMSG.
  `head` maps each kernel-owned slot to its run's first slot, which names
  the send. `pending` is the set of run heads submitted, by this
  `send_packets` or an earlier one, whose CQE has not been reaped yet.
  `sending` is true while the main thread is inside `send_packets`.
- **Receive.** Each ring buffer is in one of three states: `Ring`, `Filled`
  or `Copied`. `bufdg` is the set of datagrams filled into each buffer
  and not yet copied into a bank: one, or with `UDP_GRO` a coalesced run of
  up to `Segments` of them in the one buffer and the one CQE; the reap
  removes what it copies, so a returned buffer holds none and keeps no
  history.
  `copied` is the set of datagrams copied out into a bank. The banks are
  `fill` and `exposed`, each a set of datagram ids. `snap` is the exposed
  bank as the last Swap left it, and `gen` is the exposed generation.
  `armed` is true while the multishot RECVMSG is active in the kernel.
- **Completions.** `cq` is the set of pending CQEs: a send CQE per run (named by its head), a
  receive CQE per buffer, `Term` (a multishot CQE without F_MORE) and
  `Cancel` (the CQE of the cancel SQE). A set is enough because each CQE
  is unique while it is pending.
- **Phase:** `Running`, `Destroying` or `Freed`.
- **Calls and the budget.** `call` is the main thread's call: `none`,
  `send` (from `Submit` to `ResetIndex` or `SendReturn`) or `recv` (from
  `RecvBegin` to `Swap`). `work` counts the CQEs the current call has
  reaped, and `Budget` is how many one call may reap
  (`ROCKETNET_URING_SEND_BUDGET` and `ROCKETNET_URING_RECEIVE_BUDGET` in
  the code, one constant here). While running, the main thread reaps only
  inside a call; destroy's drain reaps outside the budget, and its rounds
  are bounded in the code (`ROCKETNET_URING_DESTROY_MAX_ROUNDS`), not here.

Six constants act as rule switches and carry the reversed witnesses:
`ResetWaitsForReap`, `ReapIntoFill`, `BoundedReap`, `RearmOnReceive`,
`RunRetiredByCqe` and `ReapWithinRoom`. All are `TRUE` in the rule.
`Room` is how many datagrams the fill bank holds between two swaps
(`ROCKETNET_URING_BANK_SIZE`); under `ReapWithinRoom` a receive CQE is
copied only while the bank has room for `Segments` more
(`rocketnet_server_uring_bank_has_room`). Otherwise, or while an earlier
receive is held, it is held: it leaves the cq, its buffer stays `Filled`
in `held`, and `CopyHeld` copies it once a swap brings room
(`rocketnet_server_uring_receive_or_hold`, `rocketnet_server_uring_copy_held`).
That is the definition `HoldWithoutRoom`; the reversed witness
`MCRNUringDriverBrokenStopAtRoom.cfg` replaces it with `MCStopAtRoom`
(`FALSE`), which leaves the receive in the cq and ends the call at it, as
the code did before the hold. The cq is a set here, so the order that made
that stop hold a send CQE behind the receive is modelled in
`RNUringReapLayout`. `ReapWithinRoom` is `FALSE` only in the
reversed witness `MCRNUringDriverBrokenUnboundedFill.cfg`, which reaps
into a full bank as the code did before the rule, when the send reap and
the receive reap between two swaps overfilled the bank and the rest was
dropped. Two more rule switches, `HoldWithoutRoom` (the seventh, above)
and `ReturnAfterLastCopy` (the eighth), and the segment bound `Segments` (2)
are definitions rather than constants, so the configurations that predate
them check the rule unchanged; a configuration replaces one with `<-`.
`ReturnAfterLastCopy` says a buffer goes back to the ring only after its
last segment is copied; the reversed witness
`MCRNUringDriverBrokenReturnBeforeSplit.cfg` replaces it with
`MCReturnAfterFirstCopy` (`FALSE`, in `MCRNUringDriver.tla`), so the reap
marks a coalesced buffer copied once its first segment is in the bank. `RunRetiredByCqe` says a
run's slots stay the kernel's until the run's one CQE; it is `FALSE` only in
the reversed witness `MCRNUringDriverBrokenFreeRunAtSubmit.cfg`, which hands
the slots after a run's head back to the staging at Submit. `RearmOnReceive` says receive_packets re-arms a
disarmed multishot (`rocketnet_server_uring_rearm_unarmed`); it is `FALSE`
only in the reversed witness `MCRNUringDriverBrokenNoRearmRetry.cfg`.

## Each action against THE DESIGN

| Action | Design sentence it models |
|---|---|
| `Reserve` | start_packet reserves the next slot with one atomic fetch_add; it returns NULL, with no step, when the staging is full |
| `Finish(s)` | finish records the bytes |
| `Abort(s)` | abort zeroes them; send_packets then skips the slot |
| `Submit` | send_packets, with the fill threads joined (no slot still `Reserved`), cuts the slots with bytes > 0 not submitted by an earlier call into runs of consecutive slots and adds one SENDMSG SQE per run, then submits; from then on the kernel owns every frame of every run. The model lets Submit pick any cut (any set of heads that includes the first slot); the code's cut is `rocketnet_uring_gso_message` (one slot per run without `UDP_SEGMENT`; with it, equal sizes to one destination, one shorter tail, bounded), which is one of them. The send call begins (`call = "send"`, `work = 0`) |
| `KernelComplete(s)` | the kernel finishes the run headed by `s` (any res, where a negative one counts SEND_DROPPED for every packet of the run) and posts the run's one send CQE; every slot of the run leaves `kown` at once |
| `KernelFill(b)` | the armed multishot RECVMSG fills a provided ring buffer and posts a receive CQE; with `UDP_GRO` the buffer holds a coalesced receive of one to `Segments` datagrams, its stride in the buffer's `UDP_GRO` cmsg |
| `KernelStop` | a CQE without IORING_CQE_F_MORE ends the multishot (ENOBUFS among the causes) |
| `Reap(c)` | the main thread reaps one CQE inside a send or receive call, counted in `work`, and under `BoundedReap` only while `work < Budget`; or in destroy's drain. A send CQE frees nothing by itself; it only removes the run's head from `pending`. A receive CQE's buffer is split at its stride (`rocketnet_uring_recvmsg_parse`, `rocketnet_coalesce_split`) and every segment is copied into the **fill** bank in the one reap, as `rocketnet_server_uring_receive_cqe` does on the one main thread (a segment dropped by size, or by a full bank as `RECEIVE_DROPPED_OVERFLOW`, is counted in place of its copy). `Term` ends the multishot. While running, the one-shot re-arm `rocketnet_server_uring_multishot_ended` attempts either succeeds (`armed` TRUE) or finds no submission entry (`RECEIVE_REARM_FAILED`, `armed` FALSE until a receive re-arms it): the model takes both. During destroy it is never re-armed |
| `CopyHeld(b)` | a held receive is copied into the fill bank, inside a call while the bank has room (the code copies the held receives first in every reap) or at once in destroy's drain; its buffer then goes back by `ReturnBuffer` |
| `ReturnBuffer(b)` | after the copy of the buffer's last segment, the buffer goes back to the ring |
| `ResetIndex` | send_packets resets the reservation index to 0 only when no slot is `Kernel` owned and every send submitted has its CQE reaped; the call returns |
| `SendReturn` | send_packets returns with some send still without its CQE, once its budget is spent or the cq is empty after its one wait: the index and every slot stay as they are, and a later call retires the rest (`SEND_DEFERRED`). Only under `BoundedReap` |
| `RecvBegin` | receive_packets begins; its reaps count against the budget. It is enabled while `gen < MaxSteps` or while the multishot is disarmed, so a re-arm is never cut off by the generation bound |
| `Swap` | the end of receive_packets, once its reap stops (the cq is empty, or under `BoundedReap` the budget is spent): the old fill bank becomes the exposed bank and the new fill bank is emptied (`gen` stops at `MaxSteps`). Under `RearmOnReceive` a disarmed multishot is armed again, as `rocketnet_server_uring_rearm_unarmed` does between the reap and the submit |
| `TransportReads` | the transport reads the views of the exposed bank. A read changes no driver state, so here it is a stutter; what the read relies on is ExposedStable |
| `Destroy` | cancels every multishot receive: the cancel posts its own CQE, and an armed multishot ends with a `Term` |
| `Free` | destroy's drain ends: once no slot is kernel owned, the cq is empty and the multishot is disarmed, slots, banks and the ring are freed |
| `Done` | the driver is gone; the stutter at the end of a run |

## Properties

Invariants:

- `TypeOK`
- `NoReuseWhileKernelOwned`: a slot the staging holds (`Free` for the next
  Reserve, or `Reserved` or `Finished` by a fill thread) is never in
  `kown`, whichever send of a run carries it, so Reserve never returns a
  slot the kernel owns. `Free` joined the list with send runs: a slot freed
  early is the defect before any Reserve reaches it.
- `ExposedStable`: `exposed = snap`, so the exposed bank never changes
  between two Swaps.
- `BufferReturnedAfterCopy`: a buffer is back in `Ring` only after every
  datagram it carried is in `copied`: with `UDP_GRO`, after the copy of its
  last segment.
- `FreedOnlyWhenDrained`: in `Freed` there is no kernel-owned slot, no
  `Filled` buffer, and the cq is empty.
- `ReapWithinBudget`: `work <= Budget`, so every Reap step of a send or
  receive call ends within the call's budget, however many CQEs the kernel
  keeps posting.
- `FillWithinRoom`: `Cardinality(fill) <= Room`, so the fill bank never
  holds more than it has room for, whichever call reaps and however many
  receives the kernel posts between two swaps.
- `HeldStillFilled`: a held receive's buffer stays `Filled` until its copy.

Action property:

- `NoCallEndsBehindRoom`: no running call ends while a send CQE waits in
  the cq and the call has budget left.

Liveness, under `SpecFair`:

- `SubmittedEventuallyFree`: every `Kernel` slot is eventually `Free`.
- `DestroyEventuallyFrees`: `Destroying ~> Freed`.
- `ReceiveLiveness`: a disarmed running multishot is re-armed by a receive,
  or destroy ends the run (`phase = "Running" /\ armed = FALSE` leads-to
  `armed = TRUE \/ phase # "Running"`). A receive with no retry never re-arms,
  a silent permanent receive stall.

`SpecFair` gives weak fairness to the following, strong where marked:

- **`KernelComplete(s)`, per slot.** The kernel completes every send; it is enabled only for a run's head.
- **`Reap(c)`, per CQE.** A CQ is FIFO, so a pending CQE is never starved
  by later ones. With a single disjunctive fairness on Reap, TLC could pick
  `Term` forever while a send CQE waits.
- **`Reap(SendCqe(s))`, strongly.** The budget disables every Reap at the
  end of each call, so weak fairness no longer reaches a send CQE: with
  weak fairness only, TLC found a loop of one-CQE calls that each reaped a
  fresh `Term` (KernelStop, re-arm, KernelStop) while a send CQE waited,
  failing `SubmittedEventuallyFree`. In the code the CQ is FIFO and the
  send CQE is older than every later `Term`, so the next call reaps it
  first: the loop is an artifact of the model's set-valued cq, and strong
  fairness on the send CQEs states the FIFO order the code has.
- **`ReturnBuffer(b)`.**
- **`Finish(s) \/ Abort(s)`.** The game finishes what it reserves.
- **`Submit`, strongly.** The game keeps calling `send_packets`, so a
  deferred send is retired by a later call. Weak fairness is not enough
  once receive re-arms: with weak fairness only, TLC found a loop of
  receive calls (`KernelStop`, `RecvBegin`, `Reap(Term)`, `Swap` re-arms)
  in which `Submit` is enabled only between calls, never continuously, so
  a `Kernel` slot was never freed and `SubmittedEventuallyFree` failed. The
  game loop calls `send_packets` every tick, whatever the receive side
  does; strong fairness on `Submit` states that.
- **`RecvBegin`, strongly.** The game keeps calling `receive_packets`.
  `RecvBegin` is disabled during every send call, so weak fairness would
  not make a receive (and its re-arm) happen.
- **`ResetIndex`, `SendReturn` and `Swap`.** A call that may end does end.
- **`Free`.** This is destroy's drain.

One assumption is not a fairness condition: under `RearmOnReceive` the
re-arm in `Swap` always succeeds. In the code
`rocketnet_server_uring_rearm_unarmed` can also find no submission entry,
counted `RECEIVE_REARM_FAILED`, and the queue stays disarmed for that call.
`rocketnet_server_uring_get_sqe` submits and asks again before it fails, so
only a submission queue that stays full fails it, and the next
receive_packets tries again. `ReceiveLiveness` holds under this assumption:
receive is re-armed once some receive call finds an entry.

The model's `armed` is the kernel's view: `KernelStop` clears it when the
multishot ends, before the `Term` is reaped. The code's `queue->armed`
stays true until `rocketnet_server_uring_multishot_ended` reaps the `Term`,
so `rocketnet_server_uring_rearm_unarmed` skips a queue whose `Term` is
still in the completion queue. When a call's budget is spent with the
`Term` still pending, the model's `Swap` re-arms one call earlier than the
code does, and the model has no step in which that call leaves the queue
disarmed. The later reap of the pending `Term` takes both outcomes in the
model, so a failed one-shot re-arm disarms it again and the next call's
`Swap` re-arms it, as the code does. The difference is at most one receive
call in when the re-arm lands; `ReceiveLiveness` is not affected by it.

## What the model leaves out

- Byte layout, the msghdr, iovec and `UDP_SEGMENT` control contents, and
  address conversion (the msghdr of a run is unit tested in
  `test_uring_gso_message_stride_and_tail`).
- Which cut of the slots into runs the code makes: the model takes every
  cut into consecutive runs. A live segmentation refusal (`GSO_FALLBACKS`)
  only changes the cut of later calls, so it is not modelled.
- The GSO-style SO_REUSEPORT socket group. The N queues are folded into
  one multishot receive. Each queue arms and re-arms on its own in the same
  way, and the banks are shared.
- SQ-full handling and partial submits. The ENOBUFS counter and the
  SEND_DROPPED counter are counts only, so they are not modelled.
- How the fill threads share the work. Their concurrency reaches the model
  only through the single fetch_add.
- The two send phases. The code stages into one of
  `ROCKETNET_URING_SEND_PHASES` phases of `ROCKETNET_URING_SEND_QUEUE_SIZE`
  slots, and resets each phase on its own once it is complete; the slots
  here are one phase, and `ResetIndex` is that phase's reset. The phases'
  layout, the fill moving to a complete phase, and the phase a send's
  completion is charged to are modelled in `RNUringSendPhases`
  (`README-RNUringSendPhases.md`).

## The small instance and the runs

`MCRNUringDriver.tla` extends the module and adds two definitions,
`MCReturnAfterFirstCopy` and `MCStopAtRoom`, the witnesses' replacements
for `ReturnAfterLastCopy` and `HoldWithoutRoom`. Every case uses `Slots = 2`, `Buffers = 2`,
`MaxSteps = 2` (two datagrams, two bank swaps), `Budget = 1` (a call reaps
one CQE, so deferral is exercised at every call) and `Segments = 2`. The
eleven cases form group `rnuringdriver` in `CASES.tsv`. The control checks
the invariants and the action property; `MCRNUringDriverLiveSends.cfg` and
`MCRNUringDriverLiveReceive.cfg` check the liveness properties, each in its
own run budget. With two slots,
Submit can send both slots as one run or as two sends; with two segments,
the kernel can fill both datagrams into one buffer as one coalesced receive.

To run them from the rocketnet checkout, with `$job`, `$jar` and `$java`
as in `proofs/tla/README.md`:

```sh
for i in $(seq 0 10); do
  "$job/tlacheck" run --root proofs --jar "$jar" --java "$java" \
    --group rnuringdriver --shards 11 --shard "$i" --workers 2 --timeout 110s \
    --dir "$job/rnuringdriver-$i"
done
"$job/tlacheck" merge --root proofs --keep proofs/tla/RUNS.tsv \
  --out "$job/RUNS.tsv" "$job"/rnuringdriver-*/RUNS.tsv
```

`$java` is an absolute path to the java binary, and `--root` is `proofs`, the
directory that holds `tla/`; `--root .` finds no models.

Outcomes measured on a 17-cpu linux-amd64 bench (Java 21.0.12.1, the pinned
JAR, 2026-10-31, the records in `RUNS.tsv`):

| Case | Expected | Exit | Generated | Distinct | Seconds |
|---|---|---:|---:|---:|---:|
| `MCRNUringDriver.cfg` | pass (8 invariants, 1 action property) | 0 | 479006 | 122677 | 4.4 |
| `MCRNUringDriverLiveSends.cfg` | pass (`SubmittedEventuallyFree`) | 0 | 479006 | 122677 | 75.2 |
| `MCRNUringDriverLiveReceive.cfg` | pass (`DestroyEventuallyFrees`, `ReceiveLiveness`) | 0 | 479006 | 122677 | 64.3 |
| `MCRNUringDriverBrokenResetBeforeReap.cfg` | `NoReuseWhileKernelOwned` | 12 | 350 | 194 | 1.1 |
| `MCRNUringDriverBrokenOneBank.cfg` | `ExposedStable` | 12 | 154 | 91 | 1.1 |
| `MCRNUringDriverBrokenUnboundedReap.cfg` | `ReapWithinBudget` | 12 | 1487 | 684 | 1.2 |
| `MCRNUringDriverBrokenNoRearmRetry.cfg` | `ReceiveLiveness` | 13 | 422554 | 109757 | 31.3 |
| `MCRNUringDriverBrokenFreeRunAtSubmit.cfg` | `NoReuseWhileKernelOwned` | 12 | 743 | 369 | 1.2 |
| `MCRNUringDriverBrokenReturnBeforeSplit.cfg` | `BufferReturnedAfterCopy` | 12 | 561 | 283 | 1.2 |
| `MCRNUringDriverBrokenUnboundedFill.cfg` | `FillWithinRoom` | 12 | 157 | 93 | 1.1 |
| `MCRNUringDriverBrokenStopAtRoom.cfg` | `NoCallEndsBehindRoom` | 13 | 36140 | 12639 | 2.1 |

The liveness cases run inside the 110 s record budget with less margin than
the control; a loaded or slower bench takes longer.

## Counterexamples, checked by hand against THE DESIGN

**BrokenResetBeforeReap** (`ResetWaitsForReap = FALSE`): ResetIndex may run
right after Submit. TLC printed five states:

1. Init.
2. `Reserve`: slot 1 is `Reserved`, idx = 1.
3. `Finish(1)`: slot 1 is `Finished` with bytes.
4. `Submit`: slot 1 is `Kernel`, kown = {1}, pending = {1}.
5. `ResetIndex`: idx = 0 and every slot is `Free`, but kown and pending
   are still {1}: slot 1 is `Free` for the next `Reserve` while the kernel
   owns it. (Before `Free` was in the invariant the trace went one state
   further, to the `Reserve` that hands slot 1 out.)

By hand, against the design: send_packets submitted slot 1's SENDMSG, and
until its CQE the kernel owns slot 1's frame and msghdr. A reset before the
reap lets the next start_packet's fetch_add return slot 1, and a fill
thread then writes the frame the kernel is still sending. That is the
corruption the design rules out with "only then does it reset the
reservation index to 0". The trace is a real defect of the broken rule, not
a model artifact.

**BrokenOneBank** (`ReapIntoFill = FALSE`): Reap copies a receive CQE into
the exposed bank. TLC printed four states:

1. Init.
2. `Submit`: a send_packets call with nothing staged begins.
3. `KernelFill(1)`: buffer 1 is `Filled` with datagram 1, and its receive
   CQE is in cq.
4. `Reap`: inside that call, buffer 1 is `Copied` and exposed = {1}, while
   snap = {} and gen = 0.

By hand, against the design: the transport is reading the views of the
exposed bank of generation 0, and no receive_packets has run. Under the
broken rule, a datagram reaped by the main thread lands in that bank, so
the bank the transport is reading changes under it. The design forbids
this with "Receive CQEs ... always go to the fill bank". This is a real
defect.

**BrokenUnboundedReap** (`BoundedReap = FALSE`): a call reaps until the cq
is empty, the loop `rocketnet_server_uring_reap_available` ran before the
budget. TLC printed six states:

1. Init.
2. `Submit`: a send_packets call begins (`work = 0`).
3. `KernelFill(1)` and 4. `KernelFill(2)`: two datagrams arrive, two
   receive CQEs are in cq.
5. `Reap`: `work = 1`.
6. `Reap`: `work = 2`, past `Budget = 1`.

By hand, against the code before the budget: the same two CQEs reaped in
one call is what `reap_available`'s `while ( true )` did in receive_packets
and in a send_packets that submitted anything (an empty send_packets reaped
nothing there; the model lets any call reap, which over-approximates it).
With the kernel posting a CQE per arriving datagram and the buffer going
back to the ring at each reap, that loop has no bound: the defect the
budget removes. The trace is a real defect of the broken rule.

**BrokenNoRearmRetry** (`RearmOnReceive = FALSE`): receive_packets never
re-arms. TLC printed a lasso of 21 states: in a first receive call the
kernel fills a buffer, the call reaps it, `KernelStop` ends the multishot
(`armed` FALSE) and the call's `Swap` takes `gen` to 1 without re-arming;
the second receive call reaps the `Term` with the one-shot re-arm finding
no entry, and its `Swap` takes `gen` to `MaxSteps`, again without
re-arming. The run then loops through a send call (`Submit`, `ResetIndex`)
and receive calls (`RecvBegin`, `Swap`) with `armed` FALSE in every state
while the phase stays `Running`. `ReceiveLiveness` fails (exit 13).

By hand, against the code before the fix: the only re-arm was the one in
the CQE handler at the end of a multishot. When `rocketnet_server_uring_arm`
finds no submission entry there, the queue stays disarmed and
receive_packets never looks at it again, so the queue receives nothing for
the rest of the run. The trace is that defect.

**BrokenFreeRunAtSubmit** (`RunRetiredByCqe = FALSE`): Submit hands the
slots of a run after its head back to the staging. TLC printed six states:

1. Init.
2. `Reserve`: slot 1 is `Reserved`, idx = 1.
3. `Reserve`: slot 2 is `Reserved`, idx = 2.
4. `Finish(1)`: slot 1 is `Finished` with bytes.
5. `Finish(2)`: slot 2 is `Finished` with bytes.
6. `Submit` with the one head {1}: both slots go out as one run, kown =
   {1, 2}, head = <<1, 1>>, pending = {1}; slot 1 is `Kernel` but slot 2 is
   `Free` while 2 is in kown. `NoReuseWhileKernelOwned` fails (exit 12).

By hand, against the code: rocketnet_server_uring_send_packets builds slot
1's msghdr over `send.iovecs[1..2]`, which point at the frames of slots 1
and 2, and the kernel reads both frames until the run's one CQE. The code
keeps every slot of the run behind the reservation index, because
`sends_pending` counts the run's SENDMSG and ResetIndex waits for it to
reach zero; a driver that counted slot 2 retired at submit (as a send that
copies at submit would allow) could reset the index with the run in flight
and hand slot 2's frame to a fill thread. The trace is that defect.

**BrokenReturnBeforeSplit** (`ReturnAfterLastCopy <- MCReturnAfterFirstCopy`):
the reap of a coalesced buffer copies its first segment and marks the
buffer copied. TLC printed the trace to `BufferReturnedAfterCopy`: Init;
`Submit` (a send_packets call with nothing staged begins); `KernelFill(1)`
with both datagrams, bufdg = <<1..2, {}>>; `Reap(RecvCqe(1))`, copied =
{1}, bufdg = <<{2}, {}>>, buffer 1 `Copied`; `ReturnBuffer(1)`: buffer 1
is `Ring` still holding datagram 2, not yet in a bank, and the invariant
fails (exit 12).

By hand, against the code: rocketnet_server_uring_receive_cqe loops over
every segment rocketnet_coalesce_split cut from the buffer, copying each
(or counting it dropped) into the fill bank, and only after the loop calls
io_uring_buf_ring_add. A transport that handed the buffer back after the
first segment would let the kernel refill it with a new coalesced receive
while the rest of the old one was still to be copied out of it: the
datagrams after the first would be read from bytes the kernel is
rewriting. The trace is that defect.

**BrokenStopAtRoom** (`HoldWithoutRoom <- MCStopAtRoom`): TLC printed the
trace to `NoCallEndsBehindRoom`: Init; `Reserve`; `Finish(1)`; `Submit`
(pending = {1}); `KernelComplete(1)` posts the send CQE; `KernelFill(1)`
and `KernelFill(2)`, one datagram each; `Reap(RecvCqe(1))` copies datagram
1 into the fill bank, which then has no room for a coalesced receive of
two; `SendReturn` on the spent budget; a second `Submit` with nothing new
begins a send call with `work = 0`; `SendReturn` again, by
`RecvWaitsForRoom`, with the send CQE still in the cq: the action property
fails (exit 13).

By hand, against the code before the hold: the cq is a set here, so this
trace posts the send CQE before the receives, an order in which the code's
FIFO reap would have taken the send CQE first. The code's defect is the
same end of the call with the send CQE posted behind a receive the bank
has no room for: the reap peeked the completion queue oldest first and
broke at that receive, so `send_packets` returned with `sends_pending`
above 0 and the index unchanged. That ordered trace is
`MCRNUringReapLayoutBrokenStopAtRoom.cfg` (README-RNUringReapLayout.md).
With the hold the receive leaves the queue and the send CQE is reaped in
the same call.

Each witness fails on exactly its declared invariant or property (exit 12
for the invariants, exit 13 for `ReceiveLiveness` and
`NoCallEndsBehindRoom`), and the controls pass every invariant, the action
property and all three liveness properties.
