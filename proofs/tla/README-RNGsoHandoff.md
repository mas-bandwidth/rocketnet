# RNGsoHandoff: the GSO server transport's bank handoff

`RNGsoHandoff.tla` models the cross-thread bank handoff of the Linux GSO
server transport, `sdk/rocketnet_server_gso_linux.cpp`, for one queue:
its three receive buffers, its three transmit banks, its worker's send step
(`rocketnet_server_gso_send_step`, each pass of `rocketnet_server_gso_worker`
sends, then drains) with a full socket and the shutdown retry bound, and
stop, join and free. Every queue has its own banks, lane mutex and worker,
so the model holds for each; the worker's poll and main's eventfd wake are
folded. The per-queue staging layout is `RNGsoQueueBankLayout.tla`; the
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
- `TxInOrder`, `RxInOrder`: packets are sent and consumed in order; a gap
  below a sent packet is a counted drop.
- `MainNeverWaits`: main only try-acquires the queue's send lane mutex.
- `RxAccounted`: every datagram the receive worker staged was consumed by
  main or is still banked (the tail's unread part, main's buffer past its
  cursor, the worker's buffer when it is another), and after `Join` every one not consumed is in
  `rxStopped`, the `RECEIVE_DROP_STOPPED` count. `StopCount` is that count,
  made in the code by the first `rocketnet_server_gso_stopped` call that
  finds the worker exited (`rocketnet_server_gso_count_stopped`); destroy
  calls stopped before it joins, and nothing moves a receive buffer once
  both workers exited and main stopped, so the model makes it in `Join`:
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
`counter_send_dropped += message_num_packets[sent]` (line 1982), the
unroutable run (line 1832), and `rocketnet_server_gso_resend_plain`
(line 1732). `ErrorBound` (one) bounds these drops per behaviour, as
`MaxTx` bounds staging, so the checks fit the run budget; one drop reaches
every place a hard error can land.

## Left out

- **Destinations and the segmentation tier.** Packets are integers with no
  destination, and the model has no UDP_SEGMENT feature state, so no
  property can say that one client's refusals leave another's send mode
  alone. The code's tier is transport-wide: `segment_consecutive_refusals`
  is one counter (`rocketnet_server_gso_linux.cpp` lines 1931-1937, reset
  only by an accepted segmented send at line 1912), and past
  `ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD` (8,
  `rocketnet_server_gso.h` line 140) `rocketnet_server_gso_send_bank`
  clears `ROCKETNET_SERVER_GSO_FEATURE_SEGMENT` for every client (line
  435). This is a known code risk whose fix is separate work; the
  destination dimension belongs in this model when that fix lands.
- A failed receive takes the stall path in the code and is not modelled
  separately (`test_server_gso_receive_backoff` covers it).

## Cases

The 2026-11-13 run on amd-2 (native Linux, the fourteen cases of the two
stale groups at once), the pinned `tlacheck` and TLC jar, one shard per
case, two workers; the records in `RUNS.tsv`. The receive half changed that
day (the worker commits its count at the end of each pass and main reads
the worker's buffer in place), which took the main case from 4762152
distinct states to 4056648.

| Config | Expected | Distinct states | Seconds | What it shows |
|---|---|---|---|---|
| `MCRNGsoHandoff.cfg` | pass | 4056648 | 45.4 | Every invariant, five datagrams, four staged packets. |
| `MCRNGsoHandoffLive.cfg` | pass | 9944 | 10.6 | Invariants, `StopCompletes`, `ShutdownCompletes`; one datagram and two staged packets (three before the hard-error drop, which took it to 81 s). |
| `MCRNGsoHandoffBrokenPublishWhileHeld.cfg` | `NoPublishWhileHeld` | 9 | 2.1 | `PublishWhileHeld = TRUE`: the worker writes into the buffer main reads after leaving it. |
| `MCRNGsoHandoffBrokenFullDrops.cfg` | `SentOrHeld` | 3952 | 4.2 | `FullDrops = TRUE`: a full socket drops the rest of its batch. |
| `MCRNGsoHandoffBrokenCarryBehind.cfg` | `TxInOrder` | 61578 | 11.5 | `CarryBehind = TRUE`: the unsent rest queues behind newer banks. |
| `MCRNGsoHandoffBrokenNoBackoff.cfg` | `RxBacksOff` | 181 | 2.5 | `NoBackoff <- TRUE`. |
| `MCRNGsoHandoffBrokenDropBeforeShutdown.cfg` | `FullSocketDropsOnlyAfterShutdown` | 7699 | 3.2 | `DropBeforeShutdown <- TRUE`: refusals count before shutdown. |
| `MCRNGsoHandoffBrokenBoundOnQuitOnly.cfg` | `ShutdownCompletes` | 10080 | 5.3 | `BoundOnQuitOnly <- TRUE`. |
| `MCRNGsoHandoffBrokenMainBlocks.cfg` | `MainNeverWaits` | 21 | 1.8 | `MainBlocks = TRUE`: main waits on the lane mutex. |
| `MCRNGsoHandoffReachErrorDropRunning.cfg` | `NotReachedErrorDropRunning` | 508 | 2.5 | Reachability: a counted hard-error drop before shutdown. |
| `MCRNGsoHandoffBrokenUncountedErrorDrop.cfg` | `SentOrHeld` | 508 | 2.1 | `CountErrorDrops <- FALSE`: a hard error drops uncounted. |
| `MCRNGsoHandoffBrokenStopTail.cfg` | `RxAccounted` | 5140 | 3.4 | `CountTail <- FALSE`: the stop count forgets the unread tail. |

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
  the tail with datagram 3 unread; stop is asked, both workers exit, and
  `Join` makes the stop count without the tail, one short of `arrived`.
  In the code `rocketnet_server_gso_count_stopped` adds
  `tail_end - tail_cursor` to `receive_dropped_stopped`; dropping that
  term is the reversal.

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
