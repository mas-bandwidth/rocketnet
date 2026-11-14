# RNGsoHandoff: the GSO server transport's bank handoff

`RNGsoHandoff.tla` models the cross-thread bank handoff of the Linux GSO
server transport, `sdk/rocketnet_server_gso_linux.cpp`, for one queue:
its three receive buffers, its three transmit banks, its worker's send step
(`rocketnet_server_gso_send_step`, each pass of `rocketnet_server_gso_worker`
sends, then drains) with a full socket and the shutdown retry bound, and
stop, join and free. Every queue has its own banks, lane mutex and worker,
so the model holds for each; the worker's poll and main's eventfd wake are
folded. One worker thread per queue does both halves, each pass the send
step then the receive drain; the model runs them as two processes, the RX
worker and the TX worker, whose steps interleave freely, which includes
every order the one thread takes. At stop the thread reads no more
datagrams once it sees quit, its send step ends with stop's final bank, and
then it returns. The per-queue staging layout is `RNGsoQueueBankLayout.tla`; the
receive handoff alone, with its delivery property `DeliveredByNextCall`, is
`RNReceiveHandoff.tla` (`README-RNReceiveHandoff.md`). The
module header gives the actions and the code each one stands for;
`proofs/tla/COVERAGE.tsv` (row `server-gso-handoff`) maps them to the
functions.

## What it checks

- `RxOneOwner`, `TxOneOwner`, `TxQueuesDistinct`, `FreeIsEmpty`: every
  buffer and bank has one owner (main and the receive worker may share
  one, and a buffer is free exactly while they do), and a free one is
  empty.
- `NoPublishWhileHeld`: main reads only below a committed count, the
  worker writes only at or past it, and a buffer the worker has left (main's,
  or the unread tail) keeps the final count main was given.
- `RxBacksOff`: after a stalled pass the receive worker sleeps.
- `SentOrHeld`, `ExitSentAll`: every staged packet is sent, still held, or
  dropped and counted in `SEND_DROPPED`, for lack of socket room after
  shutdown (`txDropped`) or by a hard send error (`txErrDropped`).
- `FullSocketDropsOnlyAfterShutdown`: packets are dropped for lack of
  socket room only after shutdown has begun. Before 2026-11-11 this was
  `DroppedOnlyAfterShutdown`, stated of every drop; it held only because
  the model had no hard-error drop.
- `RxInOrder`: datagrams are consumed in the order they arrived.
- `TxInOrder`: banks leave in the order they were published, a held bank's
  unsent rest before any later bank, and a gap below a sent packet is a
  counted drop. Inside a bank the code sends in the gather's order
  (`rocketnet_send_gather`: grouped by destination, each destination's
  packets in staged order, `RNSendGatherLayout`), so it keeps each
  destination's staged order and the bank order, not one rising order
  across destinations. The model has no destinations and sends a bank in
  staged order; with `TxCap = 2` that is the gather's order for every
  destination assignment, since two slots never reorder.
- `TailDroppedOnlyWhenStalled`: after shutdown a full socket's tail is
  dropped only by the `RetryBound + 1`-th attempt in a row that sent
  nothing. An attempt that moved `held_slot` on (sent part of the tail, or
  dropped a message counted) starts the count again
  (`rocketnet_server_gso_send_step` zeroes `retries_after_quit` when
  `held_slot` passed where the attempt began), so a socket that keeps taking
  some of it never loses the rest. The ghost `stalls` counts those attempts
  apart from the model's own count, so both ways of losing the reset (count
  the progress, or leave the count where it was) fail the invariant.
- `MainNeverWaits`: main only try-acquires the queue's send lane mutex.
- `RxAccounted`: every datagram the receive worker staged was consumed by
  main or is still banked (the tail's unread part, main's buffer past its
  cursor, the worker's buffer when it is another), and after `Join` every one not consumed is in
  `rxStopped`, the `RECEIVE_DROP_STOPPED` count. `StopCount` is that count,
  made in the code by the first `rocketnet_server_gso_stopped` call that
  finds the worker exited (`rocketnet_server_gso_count_stopped`); destroy
  calls stopped before it joins, and nothing moves a receive buffer once
  the worker thread exited (both modelled halves) and main stopped, so the
  model makes it in `Join`:
  the same value, without the interleavings of a separate step against
  `SocketFills` and `SocketDrains` (a separate step took the main case past
  the 110-second budget). Main claims and consumes nothing once stop is
  asked (`RxCall`; `rocketnet_server_gso_receive_packets`
  returns at `stopping`), so nothing is counted twice.
- `RefusalsBounded`, `StopJoinsBeforeFree`, and the liveness properties
  `StopCompletes` and `ShutdownCompletes`.

Hard send errors are folded into `TxSend` and `TxSendFull`: any message the
flush gives the kernel may be refused with an errno other than a full
socket's (EPERM from a netfilter drop, ENETUNREACH, or a destination the
socket's family cannot route). The code drops it counted and goes on with
the rest: `rocketnet_server_gso_flush` at
`counter_send_dropped += message_num_packets[sent]`, the flush's unroutable
run, and `rocketnet_server_gso_resend_plain`. `ErrorBound` (one) bounds these drops per behaviour, as
`MaxTx` bounds staging, so the checks fit the run budget; one drop reaches
every place a hard error can land.

## Left out

- **Destinations and the segmentation refusal.** Packets are integers with
  no destination, and the model has no UDP_SEGMENT feature state. A refused
  segmented send is per destination in the code: each lane's
  `rocketnet_segment_refused_t` remembers the refusing destination, its run
  is resent plain, and `rocketnet_server_gso_send_bank` clears
  `ROCKETNET_SERVER_GSO_FEATURE_SEGMENT` only when
  `ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD` refusals were counted
  and no segmented send was ever accepted. That is
  `RNSegmentRefusal.tla` (`README-RNSegmentRefusal.md`); here a refusal is
  folded into the send step, which resends the run plain inside the same
  flush.
- A failed receive takes the stall path in the code and is not modelled
  separately (`test_server_gso_receive_backoff` covers it).

## Cases

The 2026-11-13 run on aws-mut-3 (native Linux, 16 cpus), the pinned
`tlacheck` and TLC jar, one shard per case, two workers; the records in
`RUNS.tsv`. The receive half changed that day (the worker commits its
count at the end of each pass and main reads the worker's buffer in place,
and a pass that stages nothing still commits a buffer main has started on
and moves on, `RxMove`), which took the main case from 4762152 distinct
states to 5291280; the shutdown count's progress reset and its ghost
`stalls` and `shortDrop` took it to 5302440.

| Config | Expected | Distinct states | Seconds | What it shows |
|---|---|---|---|---|
| `MCRNGsoHandoff.cfg` | pass | 5302440 | 66.6 | Every invariant, five datagrams, four staged packets. |
| `MCRNGsoHandoffLive.cfg` | pass | 9944 | 7.4 | Invariants, `StopCompletes`, `ShutdownCompletes`; one datagram and two staged packets (three before the hard-error drop, which took it to 81 s). |
| `MCRNGsoHandoffBrokenPublishWhileHeld.cfg` | `NoPublishWhileHeld` | 9 | 0.7 | `PublishWhileHeld = TRUE`: the worker writes into the buffer main reads after leaving it. |
| `MCRNGsoHandoffBrokenFullDrops.cfg` | `SentOrHeld` | 4565 | 1.9 | `FullDrops = TRUE`: a full socket drops the rest of its batch. |
| `MCRNGsoHandoffBrokenCarryBehind.cfg` | `TxInOrder` | 71737 | 12.2 | `CarryBehind = TRUE`: the unsent rest queues behind newer banks. |
| `MCRNGsoHandoffBrokenNoBackoff.cfg` | `RxBacksOff` | 182 | 0.9 | `NoBackoff <- TRUE`. |
| `MCRNGsoHandoffBrokenDropBeforeShutdown.cfg` | `FullSocketDropsOnlyAfterShutdown` | 9156 | 1.2 | `DropBeforeShutdown <- TRUE`: refusals count before shutdown. |
| `MCRNGsoHandoffBrokenBoundOnQuitOnly.cfg` | `ShutdownCompletes` | 23712 | 5.0 | `BoundOnQuitOnly <- TRUE`. |
| `MCRNGsoHandoffBrokenMainBlocks.cfg` | `MainNeverWaits` | 21 | 0.7 | `MainBlocks = TRUE`: main waits on the lane mutex. |
| `MCRNGsoHandoffReachErrorDropRunning.cfg` | `NotReachedErrorDropRunning` | 508 | 0.8 | Reachability: a counted hard-error drop before shutdown. |
| `MCRNGsoHandoffBrokenUncountedErrorDrop.cfg` | `SentOrHeld` | 508 | 0.8 | `CountErrorDrops <- FALSE`: a hard error drops uncounted. |
| `MCRNGsoHandoffBrokenStopTail.cfg` | `RxAccounted` | 5994 | 1.3 | `CountTail <- FALSE`: the stop count forgets the unread tail. |
| `MCRNGsoHandoffBrokenLostProgressReset.cfg` | `TailDroppedOnlyWhenStalled` | 1363 | 0.9 | `LostProgressReset <- TRUE`: an attempt that sent part of the tail still counts toward the bound. |
| `MCRNGsoHandoffBrokenProgressKeepsCount.cfg` | `TailDroppedOnlyWhenStalled` | 1734 | 0.9 | `ProgressKeepsCount <- TRUE`: an attempt that sent part of the tail leaves the count where it was. |

## Counterexamples checked against the code (2026-11-11)

- **PublishWhileHeld (2026-11-13).** The worker stages one datagram in
  the buffer it shares with main and, at the pass's end, commits it and
  moves to the free buffer (main has started on the shared one); with
  the switch it then appends a second datagram to main's buffer, past
  the committed count. In the code the worker stages only into
  `rocketnet_server_gso_worker_buffer( queue )`, the buffer
  `handoff.worker` names, and that index changes only in
  `rocketnet_receive_handoff_commit` under the mutex, to the free
  buffer, so the step the switch adds has no code.
- **StopTail (2026-11-13).** Three datagrams arrive over three passes
  and main takes one per call (`RxCall(1)`, a budget that cuts the
  call), so the second call makes the buffer holding datagrams 2 and 3
  the tail with datagram 3 unread; stop is asked, both halves of the
  worker exit, and
  `Join` makes the stop count without the tail, one short of `arrived`.
  In the code `rocketnet_server_gso_count_stopped` adds
  `tail_end - tail_cursor` to `receive_dropped_stopped`; dropping that
  term is the reversal.

- **LostProgressReset (2026-11-13).** Two packets are staged and
  published; the socket fills and the worker takes the bank; shutdown
  begins; two attempts send nothing (count 1, then 2), and a third sends
  packet 1, which with the reversal counts 3, past `RetryBound`, and
  packet 2 is dropped. In the code `rocketnet_server_gso_send_step`
  zeroes `retries_after_quit` when `held_slot` passed where the attempt
  began, so that attempt would start the count again; skipping that reset
  is the reversal.
- **ProgressKeepsCount (2026-11-13).** The same two packets and full socket
  after shutdown: two attempts send nothing (count 2), a third sends packet
  1 and, with the reversal, leaves the count at 2, and a fourth sends
  nothing (count 3, past `RetryBound`), dropping packet 2 after one stalled
  attempt in a row. In the code that is deleting
  `lane->retries_after_quit = 0;` in the progress branch of
  `rocketnet_server_gso_send_step` while keeping its `return false`.
- **ReachErrorDropRunning.** One packet is staged, main publishes it, the
  worker takes the bank and sends it; the kernel refuses the one message
  with a hard error, so `txErrDropped = <<1>>` with `shut.on` FALSE. In the
  code, `rocketnet_server_gso_send_step` calls
  `rocketnet_server_gso_send_bank`, whose `rocketnet_server_gso_flush`
  gets `sendmmsg` -1 with, say, EPERM: not EINTR, not EAGAIN, EWOULDBLOCK,
  ENOBUFS or ENOMEM, so it adds the message's packets to
  `counter_send_dropped` and goes on (`sent++`), with no test of
  `send_shutdown`.
- **UncountedErrorDrop.** The same steps with `CountErrorDrops` FALSE: the
  refused packet leaves the bank and reaches neither list, so one staged
  packet is unaccounted for. The code's counted drop above is the line
  this reverses.
- **MainBlocks.** The queue's worker takes the lane mutex (`TxWorkerLock`); main
  then reaches its publish and, with `MainBlocks`, waits for the mutex
  (`pc["main"] = "waitlock"`). In the code,
  `rocketnet_server_gso_publish_send` uses
  `rocketnet_platform_mutex_try_acquire( &lane->mutex )` on the queue's send
  lane and returns -1 (the producer retained) on contention, and
  `rocketnet_server_gso_send_idle` does the same for each lane. In the trace
  the producer is empty, which the code
  returns on before its try; the model's `TxMainLock` does not test that,
  an over-approximation that does not change the defect.
