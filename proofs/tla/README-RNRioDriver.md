# RNRioDriver

This model covers the earlier caller-driven Windows Registered I/O (RIO)
server design. Its implementation path is
`sdk/rocketnet_server_rio_windows.cpp`; its region layout and request contexts are
`sdk/rocketnet_rio_layout.h`. The driver opens one UDP socket with
`WSA_FLAG_REGISTERED_IO`, registers one region with one
`RIORegisterBuffer`, and polls one send completion queue and one receive
completion queue, with no notification and no worker thread: everything
runs on the main thread in this model.

## Scope after the I/O worker

The runtime transport now gives every RIO call to one background I/O worker
and exchanges bank indices and send-phase states with the caller under a
short mutex. `RNRioWorker.tla` (`README-RNRioWorker.md`) models that design:
the worker, the receive banks and their backpressure, the phase handoff and
stop. This module's runs remain evidence for the slot, slice and phase rules
the worker keeps (no reserve into a posted phase, free only when drained, a
refused re-post never shrinks the depth). Its caller-driven bank swap and
per-call receive bound describe the earlier design and no longer map to the
code. The action mapping and runs below retain their historical scope.

## The abstraction

- **Send slots.** Two staging phases, each `Slots` frame slots. A slot is
  `Free`, `Reserved` (a fill thread holds it), `Finished` (its bytes are
  recorded) or `Posted` (the kernel owns its frame and its address slot).
  Each phase has a reservation index (`next`) and an outstanding count.
  `sending` marks the main thread between the post and the switch of
  `send_packets`, when the fill threads are joined.
- **Send completions.** `sendCQ` is the set of completions
  `<<phase, slot, status>>` not yet dequeued. Each posted request completes
  at most once, so a set stands for the queue's bag.
- **Receive slices.** A slice is `Posted`, `Completed` (its completion is
  queued) or `Copied` (dequeued and not posted). A refused re-post leaves
  the slice `Copied` in the retry list, where the next `receive_packets`
  re-posts it; `RetryRefused = FALSE` restores the old rule that retires it.
  `held[i]` is the datagram in the slice's buffer.
- **Retry list.** `retry` is the set of slices whose re-post was refused
  and that are waiting for the next `receive_packets`. A set of slice ids
  keeps the per-slice identity the code's `retry_slices` array carries,
  without adding a fourth slice state: every slice in the set is `Copied`.
- **Datagrams.** Abstract ids `1..MaxSteps`, which bounds the receive side.
  `consumed` is every datagram copied into a bank or counted dropped.
- **Banks.** `fillBank` and `exposed`, each at most `Slices` datagrams;
  `swapped` is the exposed bank as the last `Swap` left it, and `view` is
  the datagram the transport last read from it.
- **Socket.** `Running`, `Closing` (after `closesocket`) or `Freed` (queues
  closed, buffer deregistered, region freed).

## Each action against the design

| Action | Design sentence it models |
|---|---|
| `Reserve` | start_packet reserves the next slot of the fill phase with one fetch_add; past the last slot it returns NULL (SEND_FULL, no state change) |
| `Finish(s)` | finish records the bytes |
| `Abort(s)` | abort zeroes them: the slot is never posted |
| `Post` | send_packets, fill threads joined, posts every slot of the fill phase with bytes > 0 by RIOSendEx with RIO_MSG_DEFER, then one commit; records that phase's outstanding count |
| `KernelCompleteSend(p, s)` | the kernel completes a posted send, with a good or a failed status |
| `DequeueSend(p, s)` | send_packets dequeues the send CQ without blocking and retires each completed slot of either phase (a failed status counts SEND_FAILED) |
| `SwitchPhase` | send_packets opens the other fill phase only when its outstanding count is zero; otherwise the switch stays owed and this call returns (SEND_WAITS) |
| `KernelFillSlice(i)` | the kernel completes a posted RIOReceiveEx: a datagram in the slice, or a failed status |
| `DequeueReceive(i)` | receive_packets processes at most `rio->receives` completions per call (`rocketnet_server_rio_receives` for the configured max_clients): a failed status counts RECEIVE_FAILED; else it copies the datagram into the fill bank or counts a drop, then re-posts the slice |
| `RetryRepost(i)` | receive_packets, before it dequeues: one more post of each slice in the retry list (rocketnet_rio_retry_receives); a slice refused again stays in the list and counts RECEIVE_REPOST_FAILED again, and nothing is retried after Close |
| `Swap` | after at most one receive bank is dequeued, receive_packets swaps the banks; any queued CQ tail remains for the next update |
| `TransportReads` | process_packets returns views of the exposed bank, valid until the next receive_packets |
| `Close` | closesocket completes every outstanding request with an error |
| `Drain` | destroy dequeues both CQs; nothing is re-posted |
| `Free` | RIOCloseCompletionQueue, RIODeregisterBuffer and VirtualFree, only once every posted send and receive is accounted for |
| `Done` | the driver is gone; the run stutters, so a freed driver is not a deadlock |

The fill-thread actions (`Reserve`, `Finish`, `Abort`) and the main-thread
receive actions run only outside `send_packets` (`FillOpen`). `DequeueSend`
may interleave anywhere while the socket runs, a superset of the code's
schedule, which only strengthens the safety result.

## Invariants

- **`NoReserveIntoPostedPhase`**: a slot a fill thread holds (`Reserved` or
  `Finished`) is always in a phase with no outstanding send. Reserve never
  returns a slot of a phase the kernel still owns any of.
- **`OutstandingCounts`**: each phase's outstanding count is the number of
  its `Posted` slots.
- **`CompletionsOwned`**: every queued send completion names a `Posted`
  slot, and a slice is `Completed` exactly when its completion is queued.
- **`ExposedStable`**: the exposed bank equals what the last `Swap` left,
  and a view the transport holds is in it. Only `Swap` writes the exposed
  bank; receive copies go to the fill bank.
- **`RepostOnlyAfterCopy`**: a posted slice holds no datagram; every
  delivered datagram is still in its slice or consumed; every banked
  datagram is consumed.
- **`ReceiveCallBound`**: `receiveCount` never exceeds `Slices` between swaps; a receive invocation dequeues no more than one bank.
- **`ReceiveDepthNeverShrinks`**: while the socket is `Running` every
  slice is `Posted`, `Completed`, or waiting in the retry list. A transient
  refusal never removes a slice from the receive depth.
- **`FreedOnlyWhenDrained`**: `Freed` holds only with no `Posted` slot, no
  `Posted` or `Completed` slice, and both completion queues empty. A slice
  waiting in the retry list is not posted, so the kernel does not own it
  and it blocks neither `Drained` nor `Free`.
- **`TypeOK`**: every variable has its declared type.

## Liveness

Checked under weak fairness of each slot's kernel send completion, each
slot's send dequeue, each slice's receive dequeue, `Drain` and `Free`
(`SpecFair`). The kernel's receive fills carry no fairness: a slice may
wait for traffic forever.

- **`PostedEventuallyFree`**: every `Posted` slot is eventually `Free`.
- **`CloseEventuallyFrees`**: a `Closing` socket is eventually `Freed`.

In a scratch copy, replacing `WF_vars(Drain)` with `TRUE` makes TLC report
`Temporal properties were violated` (exit 13) on the control's instance: the
liveness properties depend on the drain, as they should. That variant is
not committed.

## What the model leaves out

- The byte layout of the region, the frame and slice sizes, and the 32-bit
  offsets: `sdk/rocketnet_rio_layout.h` and its unit tests own those.
- Address conversion (SOCKADDR_INET to `rocketnet_address_t`, the one-time
  normalization of a v4-mapped source) and the size bounds: a size drop is
  the nondeterministic "counted dropped" branch of `DequeueReceive`.
- The RIO function table (WSAIoctl `SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER`
  with `WSAID_MULTIPLE_RIO`), the request queue's capacity, and the 64-bit
  request contexts (`rocketnet_rio_context`): a completion names its phase
  and slot, or its slice, directly.
- Counter values: each counter is named at the action that bumps it, and no
  property reads one.
- The order of a completion queue: completions are dequeued in any order,
  which covers the FIFO the kernel delivers.

## How to run it

With the pinned runner and JAR of `proofs/tla/README.md`, from the checkout, one
shard per case:

```sh
for i in 0 1 2 3; do
  "$job/tlacheck" run --root . --jar "$jar" --java "$java" \
    --group rnriodriver --shards 4 --shard "$i" --workers 2 --timeout 110s \
    --dir "$job/rnriodriver-$i"
done
"$job/tlacheck" merge --root . --keep proofs/tla/RUNS.tsv \
  --out "$job/RUNS.tsv" "$job"/rnriodriver-*/RUNS.tsv
```

The small instance:

| Constant | Value |
|---|---|
| `Slots` | 1 in the control, 2 in the three witnesses |
| `Slices` | 2 (also each bank's capacity) |
| `MaxSteps` | 1 |
| `SwitchWaitsDrain` | `TRUE` (`FALSE` in the switch witness) |
| `FreeWaitsDrain` | `TRUE` (`FALSE` in the free witness) |
| `RetryRefused` | `TRUE` (`FALSE` in the retire-on-refusal witness) |

One datagram keeps the liveness check inside the 110-second budget, and two
slices keep a waiting slice beside a live one. The control uses one slot per
send phase: `Slots = 2, Slices = 2, MaxSteps = 1` with the retry state did
not finish within that budget. The switch witness keeps two slots per phase
because its defect needs the reservation index past one slot. The retired
instance is a smaller one, not a passing claim.

## Runs

The five rows are in `proofs/tla/RUNS.tsv`, each measured by `tlacheck run` on a
Linux bench (Java 21.0.12.1, the pinned JAR, 110-second budget):

| Case | Outcome | Generated | Distinct | Seconds |
|---|---|---:|---:|---:|
| `MCRNRioDriver.cfg` | PASS, exit 0, no error | 70,881 | 18,464 | 5.2 |
| `MCRNRioDriverBrokenSwitchWithoutDrain.cfg` | `NoReserveIntoPostedPhase` violated, exit 12 | 6,243 | 2,446 | 1.2 |
| `MCRNRioDriverBrokenFreeBeforeDrain.cfg` | `FreedOnlyWhenDrained` violated, exit 12 | 56 | 38 | 1.0 |
| `MCRNRioDriverBrokenRetireOnRefusal.cfg` | `ReceiveDepthNeverShrinks` violated, exit 12 | 27 | 23 | 1.0 |
| `MCRNRioDriverBrokenUnboundedReceive.cfg` | `ReceiveCallBound` violated, exit 12, depth 7 | 6,523 | 2,782 | 1.2 |

## The reversed witnesses

### `MCRNRioDriverBrokenSwitchWithoutDrain.cfg`

The committed instance with `SwitchWaitsDrain = FALSE`: `SwitchPhase`
ignores the other phase's outstanding count.

- **The trace TLC printed:**
  1. `Reserve`, `Finish(1)`: slot 1 of phase 0 is finished.
  2. `Post`: slot 1 of phase 0 is posted; outstanding of phase 0 is 1.
  3. `SwitchPhase` to phase 1 (its count is 0), then `Post` of phase 1,
     which has nothing finished.
  4. `SwitchPhase` back to phase 0 while its count is still 1, and
     `Reserve` returns slot 1 of phase 0, the slot the kernel still owns.
- **Against the design, by hand:** two `send_packets` calls in a row with
  no send completion in between are an ordinary schedule (the kernel
  completes sends when it does, and the first call's post may not have
  completed by the second). The second call switches back to phase 0. If it
  does not wait for phase 0's outstanding count, phase 0's reservation
  index starts again at slot 1, and the next `start_packet` hands a fill
  thread the frame whose RIOSendEx is still in flight, so the fill thread
  writes over a datagram the kernel is sending. The design's sentence "A
  phase is opened to fill threads only when its outstanding count is zero"
  is exactly what the witness removes. The counterexample is real for that
  defect.

### `MCRNRioDriverBrokenFreeBeforeDrain.cfg`

The committed instance with `FreeWaitsDrain = FALSE`: `Free` follows
`Close` at once.

- **The trace TLC printed:**
  1. The initial state: both receive slices are posted, as at create.
  2. `Close`: both slices complete with an error; the receive completion
     queue holds two failed completions.
  3. `Free`: the region is freed with both completions still queued.
- **Against the design, by hand:** every receive slice is posted at create,
  so an outstanding RIOReceiveEx exists from the first moment. closesocket
  completes those requests with an error, but the completions are queued,
  not consumed. Freeing at once runs RIOCloseCompletionQueue and
  RIODeregisterBuffer while completions that reference the registered
  slices are still queued and before the driver has accounted for them, and
  VirtualFree releases memory the kernel's requests were bound to. The
  design's sentence "dequeue both CQs until every posted send and receive is
  accounted for; only then RIOCloseCompletionQueue, RIODeregisterBuffer and
  VirtualFree" is exactly what the witness removes. The counterexample is
  real for that defect.

### `MCRNRioDriverBrokenRetireOnRefusal.cfg`

The committed instance with `RetryRefused = FALSE`: a refused re-post
retires its slice, the old rule.

- **The trace TLC printed:**
  1. The initial state: both receive slices are posted, as at create.
  2. `KernelFillSlice(1)`: slice 1 completes with datagram 1.
  3. `DequeueReceive(1)`: the datagram is copied into the fill bank, and
     the refused re-post leaves slice 1 `Copied`; `retry` is empty.
- **Against the code, by hand:** the fixed `rocketnet_rio_complete_receive`
  counts RECEIVE_REPOST_FAILED and appends the slice to `retry_slices`
  instead of retiring it, and `rocketnet_rio_retry_receives` re-posts it on
  the next `receive_packets`. The old rule is exactly what this witness
  restores: slice 1 is neither posted nor waiting, so the receive depth
  falls from two to one while the socket still runs, and
  `ReceiveDepthNeverShrinks` fails. The counterexample is real for that
  defect.

### `MCRNRioDriverBrokenUnboundedReceive.cfg`

`Slots = 2, Slices = 2, MaxSteps = 3` with `ReceiveBounded = FALSE`:
`DequeueReceive` may run while completions keep arriving, with no per-call
limit, so `Swap` waits for an empty receive queue as the old loop did.

- **The trace TLC printed:** `KernelFillSlice` and `DequeueReceive`
  alternate three times with no `Swap`, and `receiveCount` reaches 3, past
  one bank of 2 slices; `ReceiveCallBound` fails at depth 7.
- **Against the code, by hand:** the old `rocketnet_server_rio_receive_packets`
  loop left only on a short dequeue, so a completion queue the NIC keeps
  full holds the main thread in that call. The bounded loop counts the
  completions it dequeues and stops at `rio->receives`, one bank;
  the queued tail is dequeued by the next call. The counterexample is real
  for that defect.

## Code correspondence

`sdk/rocketnet_server_rio_windows.cpp` cites this model: `SwitchPhase` in
`rocketnet_server_rio_send_packets`, which returns with the switch owed while
the other phase has outstanding sends, and `DequeueReceive` with its per-call
bound and `Swap` in `rocketnet_server_rio_receive_packets`. The
`rio-driver` row of `proofs/tla/COVERAGE.tsv` maps every action to its function.
