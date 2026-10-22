# RNUringDriver: the io_uring server driver

`RNUringDriver.tla` models the io_uring server driver
(`sdk/rocketnet_server_uring_linux.cpp`) **before the code exists**. It
follows THE DESIGN of card `rocketnet-io-uring-driver-core`. Once that card
lands, the driver cites this module, and the `io-uring-driver` row in
`COVERAGE.tsv` moves from `owed` to `current`.

## The abstraction

The driver owns one io_uring, and only the main thread uses it. The model
keeps the three state machines of the design and abstracts away the
contents:

- **Send slots.** Each slot is in one of four states: `Free`, `Reserved`,
  `Finished` or `Kernel`. A `bytes` flag says whether finish recorded bytes
  or abort zeroed them. `idx` is the reservation index. `kown` is the set of
  slots whose frame and msghdr the kernel owns, from the moment of submit to
  the moment of the send CQE. `pending` is the set of slots submitted, by
  this `send_packets` or an earlier one, whose CQE has not been reaped yet.
  `sending` is true while the main thread is inside `send_packets`.
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
- **Calls and the budget.** `call` is the main thread's call: `none`,
  `send` (from `Submit` to `ResetIndex` or `SendReturn`) or `recv` (from
  `RecvBegin` to `Swap`). `work` counts the CQEs the current call has
  reaped, and `Budget` is how many one call may reap
  (`ROCKETNET_URING_SEND_BUDGET` and `ROCKETNET_URING_RECEIVE_BUDGET` in
  the code, one constant here). While running, the main thread reaps only
  inside a call; destroy's drain reaps outside the budget, and its rounds
  are bounded in the code (`ROCKETNET_URING_DESTROY_MAX_ROUNDS`), not here.

Four constants act as rule switches and carry the reversed witnesses:
`ResetWaitsForReap`, `ReapIntoFill`, `BoundedReap` and `RearmOnReceive`.
All are `TRUE` in the rule. `RearmOnReceive` says receive_packets re-arms a
disarmed multishot (`rocketnet_server_uring_rearm_unarmed`); it is `FALSE`
only in the reversed witness `MCRNUringDriverBrokenNoRearmRetry.cfg`.

## Each action against THE DESIGN

| Action | Design sentence it models |
|---|---|
| `Reserve` | start_packet reserves the next slot with one atomic fetch_add; it returns NULL, with no step, when the staging is full |
| `Finish(s)` | finish records the bytes |
| `Abort(s)` | abort zeroes them; send_packets then skips the slot |
| `Submit` | send_packets, with the fill threads joined (no slot still `Reserved`), adds one SENDMSG SQE for each slot with bytes > 0 not submitted by an earlier call and submits; from then on the kernel owns those frames. The send call begins (`call = "send"`, `work = 0`) |
| `KernelComplete(s)` | the kernel finishes the send (any res, where a negative one counts SEND_DROPPED) and posts the send CQE |
| `KernelFill(b)` | the armed multishot RECVMSG fills a provided ring buffer and posts a receive CQE |
| `KernelStop` | a CQE without IORING_CQE_F_MORE ends the multishot (ENOBUFS among the causes) |
| `Reap(c)` | the main thread reaps one CQE inside a send or receive call, counted in `work`, and under `BoundedReap` only while `work < Budget`; or in destroy's drain. A send CQE frees nothing by itself; it only removes the slot from `pending`. A receive CQE is copied into the **fill** bank. `Term` ends the multishot. While running, the one-shot re-arm `rocketnet_server_uring_multishot_ended` attempts either succeeds (`armed` TRUE) or finds no submission entry (`RECEIVE_REARM_FAILED`, `armed` FALSE until a receive re-arms it): the model takes both. During destroy it is never re-armed |
| `ReturnBuffer(b)` | after the copy, the buffer goes back to the ring |
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
- `NoReuseWhileKernelOwned`: a slot held by a fill thread (`Reserved` or
  `Finished`) is never in `kown`, so Reserve never returns a slot the
  kernel owns.
- `ExposedStable`: `exposed = snap`, so the exposed bank never changes
  between two Swaps.
- `BufferReturnedAfterCopy`: a buffer is back in `Ring` only after its
  datagram is in `copied`.
- `FreedOnlyWhenDrained`: in `Freed` there is no kernel-owned slot, no
  `Filled` buffer, and the cq is empty.
- `ReapWithinBudget`: `work <= Budget`, so every Reap step of a send or
  receive call ends within the call's budget, however many CQEs the kernel
  keeps posting.

Liveness, under `SpecFair`:

- `SubmittedEventuallyFree`: every `Kernel` slot is eventually `Free`.
- `DestroyEventuallyFrees`: `Destroying ~> Freed`.
- `ReceiveLiveness`: a disarmed running multishot is re-armed by a receive,
  or destroy ends the run (`phase = "Running" /\ armed = FALSE` leads-to
  `armed = TRUE \/ phase # "Running"`). A receive with no retry never re-arms,
  a silent permanent receive stall.

`SpecFair` gives weak fairness to the following, strong where marked:

- **`KernelComplete(s)`, per slot.** The kernel completes every send.
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

- Byte layout, the msghdr and iovec contents, and address conversion.
- The GSO-style SO_REUSEPORT socket group. The N queues are folded into
  one multishot receive. Each queue arms and re-arms on its own in the same
  way, and the banks are shared.
- SQ-full handling and partial submits. The ENOBUFS counter and the
  SEND_DROPPED counter are counts only, so they are not modelled.
- How the fill threads share the work. Their concurrency reaches the model
  only through the single fetch_add.

## The small instance and the runs

`MCRNUringDriver.tla` extends the module with no overrides. Every case
uses `Slots = 2`, `Buffers = 2`, `MaxSteps = 2` (two datagrams, two bank
swaps) and `Budget = 1` (a call reaps one CQE, so deferral is exercised at
every call). The five cases form group `rnuringdriver` in `CASES.tsv`.

To run them from the rocketnet checkout, with `$job`, `$jar` and `$java`
as in `tla/README.md`:

```sh
for i in 0 1 2 3 4; do
  "$job/tlacheck" run --root . --jar "$jar" --java "$java" \
    --group rnuringdriver --shards 5 --shard "$i" --workers 2 --timeout 110s \
    --dir "$job/rnuringdriver-$i"
done
"$job/tlacheck" merge --root . --keep tla/RUNS.tsv \
  --out "$job/RUNS.tsv" "$job"/rnuringdriver-*/RUNS.tsv
```

Outcomes measured on a linux-amd64 bench (Java 21.0.12.1, the pinned JAR,
2026-10-20):

| Case | Expected | Exit | Generated | Distinct | Seconds |
|---|---|---:|---:|---:|---:|
| `MCRNUringDriver.cfg` | pass (6 invariants, 3 properties) | 0 | 606276 | 159168 | 84.7 |
| `MCRNUringDriverBrokenResetBeforeReap.cfg` | `NoReuseWhileKernelOwned` | 12 | 763 | 370 | 0.8 |
| `MCRNUringDriverBrokenOneBank.cfg` | `ExposedStable` | 12 | 112 | 67 | 0.6 |
| `MCRNUringDriverBrokenUnboundedReap.cfg` | `ReapWithinBudget` | 12 | 1011 | 473 | 0.7 |
| `MCRNUringDriverBrokenNoRearmRetry.cfg` | `ReceiveLiveness` | 13 | 531357 | 141455 | 20.3 |

The control's 84.7 s is inside the 110 s record budget; before the re-arm
rule (two liveness properties, weak fairness on `Submit`) it measured 53.7 s.

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

Each witness fails on exactly its declared invariant or property (exit 12
for the invariants, exit 13 for `ReceiveLiveness`), and the control passes
every invariant and all three liveness properties.
