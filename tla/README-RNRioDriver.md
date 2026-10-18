# RNRioDriver

The Windows Registered I/O (RIO) server driver, modelled from its design
before its code exists. The driver is `sdk/rocketnet_server_rio_windows.cpp`
(owed by the driver card); its region layout and request contexts are
`sdk/rocketnet_rio_layout.h`. The driver opens one UDP socket with
`WSA_FLAG_REGISTERED_IO`, registers one region with one
`RIORegisterBuffer`, and polls one send completion queue and one receive
completion queue, with no notification and no worker thread: everything
runs on the main thread.

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
  queued) or `Copied` (dequeued and not posted; a slice whose re-post failed
  stays `Copied`, retired). `held[i]` is the datagram in the slice's buffer.
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
| `SwitchPhase` | send_packets switches the fill phase to the other phase, opening it only when its outstanding count is zero; while it waits it polls the send CQ (SEND_WAITS) |
| `KernelFillSlice(i)` | the kernel completes a posted RIOReceiveEx: a datagram in the slice, or a failed status |
| `DequeueReceive(i)` | receive_packets, per completion: a failed status counts RECEIVE_FAILED; else the datagram is copied into the fill bank, or counted dropped (RECEIVE_DROPPED_SIZE, RECEIVE_DROPPED_OVERFLOW); then the slice is re-posted, or retired when the re-post fails (RECEIVE_REPOST_FAILED) |
| `Swap` | once the receive CQ is empty, receive_packets swaps the banks: the old fill bank becomes the exposed bank |
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
- **`FreedOnlyWhenDrained`**: `Freed` holds only with no `Posted` slot, no
  `Posted` or `Completed` slice, and both completion queues empty.
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

With the pinned runner and JAR of `tla/README.md`, from the checkout, one
shard per case:

```sh
for i in 0 1 2; do
  "$job/tlacheck" run --root . --jar "$jar" --java "$java" \
    --group rnriodriver --shards 3 --shard "$i" --workers 2 --timeout 110s \
    --dir "$job/rnriodriver-$i"
done
"$job/tlacheck" merge --root . --keep tla/RUNS.tsv \
  --out "$job/RUNS.tsv" "$job"/rnriodriver-*/RUNS.tsv
```

The small instance:

| Constant | Value |
|---|---|
| `Slots` | 2 |
| `Slices` | 2 (also each bank's capacity) |
| `MaxSteps` | 1 |
| `SwitchWaitsDrain` | `TRUE` (`FALSE` in the switch witness) |
| `FreeWaitsDrain` | `TRUE` (`FALSE` in the free witness) |

One datagram keeps the liveness check inside the 110-second budget; two
slots per phase keep the reservation index past one slot, and two slices
keep a retired slice beside a live one. `Slots = 2, Slices = 2, MaxSteps = 2` did not finish within that budget.

## Runs

The three rows are in `tla/RUNS.tsv`, each measured by `tlacheck run` on a
Linux bench (Java 21.0.12.1, the pinned JAR, 110-second budget):

| Case | Outcome | Generated | Distinct | Seconds |
|---|---|---:|---:|---:|
| `MCRNRioDriver.cfg` | PASS, exit 0, no error, depth 24 | 431,147 | 81,330 | 21.4 |
| `MCRNRioDriverBrokenSwitchWithoutDrain.cfg` | `NoReserveIntoPostedPhase` violated, exit 12, depth 8 | 4,036 | 1,496 | 0.8 |
| `MCRNRioDriverBrokenFreeBeforeDrain.cfg` | `FreedOnlyWhenDrained` violated, exit 12, depth 3 | 56 | 37 | 0.7 |

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

## Citation owed

No sdk file is edited by this change. The driver card that writes
`sdk/rocketnet_server_rio_windows.cpp` cites this model from its send
switch and its destroy, and moves the `rio-driver` row of
`tla/COVERAGE.tsv` from `owed` to `current`.
