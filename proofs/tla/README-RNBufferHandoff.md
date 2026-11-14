# RNBufferHandoff

The N-buffer handoff of `sdk/rocketnet_buffer_handoff.h`, as the client
transport runs it in both directions (docs/decisions/BUFFER-HANDOFF-DECISION-20261021.md).
Every buffer has one owner: the producer, the ready queue, the consumer or the
free queue. Only indices move, inside a mutex held for the exchange alone.
The game thread only try-acquires the mutex; the I/O thread acquires it and
waits on a level-triggered wake.

| direction | producer | consumer | MaxReady | code |
|---|---|---|---|---|
| send | game thread (payload rings) | I/O thread | 2 | `rocketnet_client_io_hand_off`, `rocketnet_client_io_step`, `rocketnet_client_io_stop` |
| receive | I/O thread (datagrams) | update | 1 | `rocketnet_client_io_step`, `rocketnet_client_io_expose`, `rocketnet_client_io_return` |

The instance is three buffers of two items and five items. Both controls enable
every rule. Exact current state counts and outcomes are in `RUNS.tsv`
(group `rnbufferhandoff`); these records include the receive return path.

## Send destinations

`rocketnet_client_io_hand_off` binds the Filling ring's destination
(`io->to[ring]`, `io->to_held[ring]`) at the first attempt that finds
committed slots, before it try-acquires the mutex; a LATE result, on a busy
mutex or with no ring free, leaves the binding, and a later attempt for the
same ring keeps it; a publish clears `to_held`. The I/O thread sends every
slot of a ring to `io->to[ring]` (`rocketnet_client_io_send_ring`). The model
carries this for the send direction:

- `addr` is the address main passes; `itemTo` records the address each item
  was committed with; `dest` and `held` are `io->to` and `io->to_held`;
  `sentTo` is the destination each consumed item went to.
- `MainTryLock` and `MainLate` bind (`BindDest`, `BindHeld`); `MainLate` is
  the try-acquire that finds the mutex busy and returns LATE, a step of its
  own because the binding it leaves is visible later. `ProducerExchange`'s
  publish clears `held` for the published ring; its no-ring-free branch is
  the other LATE and keeps the binding. A newly taken Filling ring's `dest`
  is set to `none`: the code leaves a stale `io->to` there, read only after
  an attempt binds it.
- `Reset` is disconnect's protocol reset: the disconnect commits its burst
  and attempts a hand-off with the saved address first, so the Filling ring
  is empty or bound when `addr` becomes `Z`; nothing is committed after.
  Later attempts pass `Z`. (The transport itself passes the ring's own
  `io->to` once the server address is zero,
  `rocketnet_client_transport_fill_destination`; the model checks the I/O
  core's rule for the harder `to`.) `MainStop` binds an unbound ring with
  items to `addr` while it is not `Z`, as that function does.
- `Rebind` is the family race's commit (`rocketnet_client_race_commit` calls
  `rocketnet_client_io_rebind_held`): `addr` goes from `A` to the kept `B`,
  and a held ring bound to `A` is bound to `B`. Items committed while the
  race was open were for both families, so `SentToCommitted` accepts an `A`
  item sent to `B` (RNClientRace `PostCommitSlotsKept`).
- `SentToCommitted`: each consumed item was sent to the address it was
  committed with. It is in every configuration; on receive it is vacuous.

## Receive expose and return

`ConsumerExchange(main, TRUE)` is expose: release a deferred consumed buffer,
then take the oldest ready one. `ConsumerExchange(main, FALSE)` is a successful
return: release the consumed held buffer, take nothing, read/clear owed, and
wake after unlocking. It requires a held buffer, as `rocketnet_client_io_return`
returns immediately when none is held. `MainTryLock` already requires the
consumer to finish its view before either exchange.

A failed return can leave the held buffer for the next expose; the existing
take exchange represents that path. The model abstracts failed try-lock calls
as no exchange. After a successful return, another release-only exchange is
disabled until an expose takes a buffer, so adding return cannot create an
infinite sequence of release-only calls that starves all takes. The original
fairness on take exchanges, mutex acquisition, consumption and unlock is
unchanged; no new fairness assumption forces a successful return.

## Abstractions

- One data item stands for one packet or datagram; `Cap` stands for a ring's
  256 slots or a receive buffer's 1024 records.
- `WorkerExit` reads the ready queue directly; the code observes it empty under
  the mutex after the quit load, and drains every ready ring in the one step
  before it exits.
- `WorkerLock` and `WorkerExit` are strongly fair: a blocked acquirer gets a
  mutex that is free infinitely often, and the code's exit check follows the
  exchange that found the queue empty. Main's try-lock is strongly fair: update
  runs every frame. Stop is never forced.
- The wait's guard timeout is not modelled: 50 ms
  (`ROCKETNET_CLIENT_IO_GUARD_TIMEOUT_SECONDS`, a guard, never a cadence), or
  2 ms while a send is pending a retry
  (`ROCKETNET_CLIENT_IO_SEND_RETRY_TIMEOUT_SECONDS`). A lost wake is a
  liveness failure here, where the code would lose up to the guard.
- `owed` is read only by main's exchanges (`rocketnet_buffer_handoff_owed`,
  which also clears it), and cleared too by any successful publish
  (`rocketnet_buffer_handoff_publish`, the producer's exchange); the worker's
  send-side consumer exchange leaves it.
- `Rebind` runs only while the protocol names the losing server (`addr =
  "A"`): a commit after a `Reset` is not explored here. The commit's rebind in
  general is `RNClientRace` (`CallerCommit`).

`ExitSentAll` contributes send-direction evidence only. Its antecedent includes
`Direction = "send"`, so its entries in receive configurations are vacuous for
receive behavior; the receive delivery property is `ArrivedDelivered`.

## Reversed witnesses

Each restores one defect and fails its named property. The traces below are
TLC's, checked against the code by hand.

- `MCRNBufferHandoffBrokenMainBlocks.cfg`, `MainNeverWaits`, 3 states: the I/O
  thread takes the mutex, then update finds it held and waits. In the code,
  `rocketnet_client_io_receive_exchange` calls
  `rocketnet_platform_mutex_try_acquire` and returns false; replacing it with
  `rocketnet_platform_mutex_acquire` is this defect.
  `test_client_io_hand_off_never_waits_for_the_mutex` holds the mutex and
  checks that hand-off returns LATE and expose returns NULL.
- `MCRNBufferHandoffBrokenWorkUnderLock.cfg`, `NoWorkUnderLock`, 3 states: the
  game thread takes the mutex and writes a packet before releasing it. In the
  code no copy, clear, send or wake sits between acquire and release in
  `rocketnet_client_io_hand_off`, `rocketnet_client_io_receive_exchange`,
  `rocketnet_client_io_step` or the BASE pair; ring clears
  (`num_packets = 0`) and the wake follow the release.
- `MCRNBufferHandoffBrokenWriteAfterPublish.cfg`, `ViewStable`, 8 states: the
  I/O thread publishes buffer 0, update takes it, then the I/O thread writes
  into it. In the code the I/O thread writes only `io->buffer[io->receive_fill]`,
  and `receive_fill` changes only to the index the publish returns.
- `MCRNBufferHandoffBrokenSkipWake.cfg`, `ArrivedDelivered` (temporal), 28
  states: the I/O thread holds item 5 in its buffer with one buffer ready, so
  its publish is owed and it waits; update takes the ready buffer and clears
  the owed flag without waking it; the I/O thread waits forever. In the code
  `rocketnet_buffer_handoff_owed` is read in the same locked exchange and the
  wake follows the release; `test_client_io_step_full_buffer_waits_on_wake_only`
  and `test_client_io_step_receive_triple_buffer` check the one wake.
- `MCRNBufferHandoffBrokenReleaseBeforeSent.cfg`, `FreeIsEmpty`, 10 states:
  the I/O thread frees a ring it took before sending it. In the code the ring
  the I/O thread holds (`send_ring`) is released only at its next exchange,
  after `rocketnet_client_io_send_ring` returned.
- `MCRNBufferHandoffBrokenDropFinal.cfg`, `ExitSentAll`, 4 states: main writes
  a packet, stops, and the I/O thread exits without the Filling ring stop
  handed over. In the code `rocketnet_client_io_stop` records `send_final`
  before the quit store, and the exit branch of `rocketnet_client_io_step`
  sends it; `test_client_io_stop_hands_over_the_filling_ring` checks all three
  rings arrive in order.

- `MCRNBufferHandoffBrokenDestinationOverwrite.cfg`, `SentToCommitted`, 10
  states: (1) main commits item 1 with address A into ring 0; (2) the I/O
  thread acquires the mutex at the top of `rocketnet_client_io_step`, and
  (3) its exchange takes nothing; (4) main's hand-off binds `to[0] = A`,
  `to_held[0] = true`, and `rocketnet_platform_mutex_try_acquire` fails:
  LATE; (5) disconnect resets the protocol (its own attempt finds ring 0
  bound); (6) the next attempt passes the zeroed address and, with the
  defect, writes `to[0] = Z` (the code's `if ( !io->to_held[ring] )` keeps
  A); (7) the I/O thread releases the mutex; (8) stop finds `to_held[0]`
  set, records `send_final = 0` and stores quit; (9) the exit branch of
  `rocketnet_client_io_step` sends ring 0 to `io->to[0]`, Z: item 1 was
  committed for A. Checked by hand against `rocketnet_client_io_hand_off`,
  `rocketnet_client_io_stop` and `rocketnet_client_io_step`. In today's
  transport step (6) passes `io->to[0]` itself (`fill_destination`), so this
  defect alone is masked there; the witness shows the I/O core's guard is
  what makes the rule hold for any `to` the caller passes.
- `MCRNBufferHandoffBrokenSkipRebind.cfg`, `SentToCommitted`, 10 states: (1)
  main commits item 1 with A (race open); (2, 3) the I/O thread holds the
  mutex; (4) main's hand-off binds `to[0] = A` and returns LATE on the busy
  mutex; (5) the I/O thread releases it; (6) the race commits to B and, with
  the defect, `rocketnet_client_io_rebind_held` is skipped, leaving
  `to[0] = A`; (7) main commits item 2 with B into ring 0; (8) stop finds
  `to_held[0]` set and leaves `to[0]`; (9) the I/O thread's exit sends both
  items to A: item 2 was committed for B. In the code
  `rocketnet_client_race_commit` calls `rocketnet_client_io_rebind_held`,
  which moves `to[0]` from the losing to the kept server.

## Cases

Group `rnbufferhandoff`, bench linux-amd, 2 workers (a counterexample case
runs with one), tlacheck r1 of this change:

| config | expected | distinct | seconds | exit |
|---|---|---:|---:|---:|
| MCRNBufferHandoffSend.cfg | pass | 74,774 | 13.0 | 0 |
| MCRNBufferHandoffReceive.cfg | pass | 5,590 | 3.9 | 0 |
| MCRNBufferHandoffBrokenMainBlocks.cfg | MainNeverWaits | 12 | 1.1 | 12 |
| MCRNBufferHandoffBrokenWorkUnderLock.cfg | NoWorkUnderLock | 13 | 2.0 | 12 |
| MCRNBufferHandoffBrokenWriteAfterPublish.cfg | ViewStable | 74 | 2.3 | 12 |
| MCRNBufferHandoffBrokenSkipWake.cfg | ArrivedDelivered | 4,219 | 3.1 | 13 |
| MCRNBufferHandoffBrokenReleaseBeforeSent.cfg | FreeIsEmpty | 693 | 1.8 | 12 |
| MCRNBufferHandoffBrokenDropFinal.cfg | ExitSentAll | 33 | 1.7 | 12 |
| MCRNBufferHandoffBrokenDiscardOnStop.cfg | ExitSentAll | 33 | 1.3 | 12 |
| MCRNBufferHandoffBrokenDestinationOverwrite.cfg | SentToCommitted | 780 | 2.5 | 12 |
| MCRNBufferHandoffBrokenSkipRebind.cfg | SentToCommitted | 799 | 2.0 | 12 |

## BASE batched update refinement

`RNBufferHandoffBase.tla` models BASE separately because update may own several
buffers at once. `MCRNBufferHandoffBase.cfg` checks four buffers (`MaxReady=N-1`),
a producer that always owns one buffer, and the maximal oldest READY prefix
that fits a 1024-packet update. Each model item represents 256 packets, with
two items per buffer and five produced items. This finite quotient includes
multiple buffers drained in one update and a queued buffer that exceeds the
remaining budget; it does not enumerate all possible packet counts.

`Expose` returns any deferred cleared buffers and takes the maximal fitting
prefix under the mutex. `Process` reads and clears outside the main mutex.
`ReleaseOnly` returns the batch without taking another buffer. A failed
`MainTry` advances directly to the end of the update, retaining ownership;
the next successful expose releases those deferred buffers. Producer writes
and publication may interleave between these phases. The model checks safety,
including FIFO delivery, stable consumer views, exact ownership, empty free
buffers, the budget, maximal draining and absence of a main wait state. It
makes no fairness or bounded-latency claim about eventual successful try-locks.
The existing threaded receive model retains its wake/liveness checks.

`MCRNBufferHandoffBaseThreeBuffers.cfg` adds a matched healthy three-buffer
instance for `BrokenDeferred` (`OneOwner`) and `BrokenSingleTake`
(`MaximalDrain`). It differs from the four-buffer healthy configuration only
in `N`; both keep all ten invariants and the same capacity, item count,
packet weight and packet budget. Each of those two three-buffer witnesses
then changes only its named defect switch. The four-buffer control remains
part of the declared suite.

The `rnbufferhandoffbase` records use the same pinned runner and TLC JAR as CI.
Reversed controls restore one defect each: `BaseBrokenSingleTake` leaves a
second fitting READY buffer behind (`MaximalDrain`); `BaseBrokenBudget` takes
three buffers containing five items, exceeding 1024 (`BudgetBound`);
`BaseBrokenDeferred` forgets a consumer-owned buffer when its return try-lock
fails (`OneOwner`); `BaseBrokenMainBlocks` turns update's try-lock into a
blocking acquire, so update sits in its take phase while the I/O thread holds
the mutex (`MainNeverWaits`, 11 distinct states); `BaseBrokenWorkUnderLock`
keeps the mutex through `Expose` and reads and clears the buffers in `Process`
under it (`NoWorkUnderLock`, 13 distinct states). Each must produce the named
invariant counterexample. The code's expose and return
(`rocketnet_client_update_expose_legacy_receive`,
`rocketnet_client_update_return_legacy_receive`) only try-acquire and release
the mutex before the buffers are read or cleared.

Measured on Linux with the CI-pinned runner `0c97d5958bf2b3cb1ad24e6e1b58f5b04e70347a`:
control 3,211 generated / 1,825 distinct states; all ten invariants pass.
The single-take witness is 9 states (buffer 1 remains READY after buffer 0
was taken with only 256 packets charged). The budget witness is 14 states
(three buffers, 1,280 packets). The deferred-return witness is 10 states
(buffer 0 was consumed, the worker holds the mutex, and forgetting the held
index leaves that buffer ownerless). Logs are retained on the allocated
Linux bench under `~/p3-stella-handoff/tlc-base-3/`; measured records are in
`RUNS.tsv`. `tlacheck groups --stale` returned `[]` after integration.

## IOCP stop drain

The Windows server transport follows `MainStop` and `WorkerExit`: stop transfers
its final producer, and TX sends every published bank before that producer.
`DiscardOnStop` restores the defect that clears these buffers after quit without
appending their datagrams to `consumed`. The reversed configuration
`MCRNBufferHandoffBrokenDiscardOnStop.cfg` checks `ExitSentAll`; existing
configurations keep this switch FALSE. The native
`test_iocp_transport_stop_sends_published_burst` holds TX, publishes ten loopback
datagrams, stops, releases TX and checks all ten arrive with no failed sends.
Completion workers wait for TX exit before cancelling receives individually,
so cancellation cannot abort the final sends. Kernel completion and cancellation
are outside this buffer ownership model.
