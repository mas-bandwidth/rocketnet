# RNDriverSendStaging

## XDP generation retention

The XDP submodel composes with the existing transport model below. `XdpStart`
represents a send that still has packet-slot capacity; the separate
`FailedSend` transport action continues to model packet-capacity exhaustion.
`NoRefusedSend` forbids refusal caused solely by handing every physical
buffer to a lagging worker. It does not claim infinite packet storage.

Since f2063bf7 the XDP socket stages through
`rocketnet_xdp_buffer_exchange_t` over `ROCKETNET_XDP_NUM_BUFFERS` (3)
buffers, and the model's `XdpBuffers` constant is that count (3 in every
committed configuration). One buffer is always the producer; `xdpQueue`
counts published buffers not yet recycled, ready or claimed. `Publish`
models `rocketnet_xdp_buffer_exchange_t::publish` and
`rocketnet_server_socket_send_packets`: when no replacement is free
(`xdpQueue = XdpBuffers - 1`), publication retains the producer.
`WorkerRetire` models the worker's final copy and `recycle` returning the
buffer to the free ring; `claim` moves a buffer from ready to claimed without
changing either count and is folded into it.
`rocketnet_server_transport_send_packets` retries XDP publication even when
`send_staged == send_flushed`; `RetryFlushPublishes` checks that a subsequent
flush publishes retained work after retirement.

`rocketnet_server_socket_send_packets` takes the handoff lock by a bounded
try (`rocketnet_xdp_handoff_lock`, `ROCKETNET_XDP_HANDOFF_TRIES`); when every
try fails, because the xdp thread holds the lock to claim or recycle a
buffer, the publish is put off, counted, and the producer stays staged for
the next call. `Publish` has that busy arm: the flush has run and nothing is
published. `XdpStagedEventuallyPublished` checks eventual publication under
weak fairness of retirement and of the publish with the lock taken
(`PublishWith(TRUE)`), which is the assumption that the xdp thread does not
hold the lock at every try of every flush. Fairness on the whole `Publish`
step, busy tries included, does not give it:
`MCRNDriverSendStagingBrokenLockAlwaysBusy.cfg` (`SpecFairLockAlwaysBusy`)
fails `XdpStagedEventuallyPublished` with a run whose every flush finds the
lock held. `RetryFlushPublishes` is about a flush that took the lock; a busy
try records no replacement free.

The transport and XDP submodels interleave independently and are not
composed: the transport's `sent` is a frame handed to the driver's flush,
and no state says which frames a retained XDP producer holds. This is a
bounded abstraction of generation ownership, not a proof of the C++ memory
model.

The control uses `PublishWith(TRUE)` and checks both new invariants and the
publication liveness property. The declared cases run through `tlacheck run --group rndriversendstaging`; `tlacheck merge --keep proofs/tla/RUNS.tsv` writes the
measured records. The frame and burst instance sizes remain unchanged.

### Reversed no-refusal witness

`MCRNDriverSendStagingBrokenRefusesSend.cfg` selects `SpecBrokenRefuses`, whose
publish action is `PublishWith(FALSE)`, which publishes the producer itself
when no replacement is free. TLC must reject exactly `NoRefusedSend`: start and
publish repeat until every buffer is queued, the last publish leaves
`xdpFilling = FALSE`, and the next start sets `xdpRefused = TRUE`. No worker
retirement occurs in that trace. (With two buffers the trace was depth 6; the
current three-buffer expected outcomes are recorded in RUNS.tsv.)

Checked against the code by hand: restoring unconditional publication in
`rocketnet_xdp_buffer_exchange_t::publish` permits all buffers to be queued;
the next `rocketnet_server_socket_send_packets` clears `send_flow_index` and
cannot acquire replacement storage, so
`rocketnet_server_socket_start_packet_internal` returns NULL on the next send.
With retention enabled, the second publish leaves its filling
index intact. Repeated flushes preserve the queued index; retirement followed
by another flush publishes the retained generation. The unit test
`test_xdp_transport_send_staging_reflushes_without_commit` separately exercises the actual
transport flush path twice with equal counters and a fake staged backend.

The current `rndriversendstaging` records in `RUNS.tsv` contain the control
and the expected `NoRefusedSend`, `StagedDisconnectFlushed` and
`FinishAfterResetRecorded` violations. Use that ledger for the exact input
fingerprints and measurements; the older witness trace below is historical.

### Equal-counter retry proof

The earlier exploratory no-retry TLC trace was removed from the committed
case set. The production retry remains checked directly against the code:
removing the
`ROCKETNET_TRANSPORT_REFLUSHES` capability check in
`rocketnet_server_transport_send_packets` leaves the retained generation
unpublished once `send_flushed` catches up to `send_staged`, until an
unrelated commit advances the staged counter. The unit check
`test_xdp_transport_send_staging_reflushes_without_commit` fails for that schedule.

## Transport model and historical disconnect witness

The staged send path of the server transport: the reserve/commit seam every
driver serves, the staged branch of `send_packets`, the maintenance half of
`update`, and the out-of-band `disconnect_client`. The code is
`sdk/rocketnet_server_transport.cpp`: `rocketnet_server_reserve_send_frame`,
`rocketnet_server_commit_send_frame`, `rocketnet_server_transport_update`,
`rocketnet_server_transport_send_packets`, and
`rocketnet_server_transport_disconnect_client`. The disconnect trace below
records the earlier implementation; its numeric locations are historical,
not locations in the current source.

- **The seam.** Reserve takes a staging slot for one frame; commit seals the
  frame into the driver's own staging. A reserve that exhausts packet-slot capacity
  fails and the frame is dropped; XDP generation ownership is modelled separately.
- **The publish rule.** On a staged driver a sealed frame is handed on only
  at a driver flush, and `send_packets` flushes when the staged counter
  differs from the flushed counter, during shutdown, or when the transport
  declares `ROCKETNET_TRANSPORT_REFLUSHES`. `SendPackets` is the branch of a
  transport without `REFLUSHES`, outside shutdown: it flushes exactly when
  the counters differ. XDP declares `REFLUSHES`; its retry with equal
  counters is the XDP submodel's `Publish`. The model's `Commit` combines
  sealing through the helper with the caller's staged-count bookkeeping; the
  helper alone does not increment the counter.
- **A withdrawn frame keeps its slot until the flush.**
  `rocketnet_server_commit_staged`, when the link simulator is active, hands
  the simulator the datagram and calls `io_abort_packet` (`Commit`'s second
  arm). `rocketnet_server_abort_send_frame` calls it for a frame reserved and
  never committed: maintenance that found no keep-alive or ack due
  (`Update`'s abort arm), and a disconnect seal whose write failed. On every
  staged driver `io_abort_packet` only zeroes the slot's byte count: XDP
  (`packet_info[...].packet_bytes = 0` in
  `rocketnet_server_socket_abort_packet`), io_uring
  (`rocketnet_uring_send_queues_abort`), GSO, sendmsg_x, IOCP and RIO
  (`rocketnet_rio_lane_abort_packet`: "its reservation stays taken until the
  phase is handed off"). The slot stays claimed in the producer bank, the
  flush skips it, and only the flush that hands the bank off frees it. In
  the model the frame is in `withdrawn` and in `claimed`: `claimed` counts
  against `MaxStaged` with the armed `buffer` and the outstanding
  reservation, whose slot `rocketnet_server_reserve_send_frame` claims
  (`Used`), so three withdrawn
  frames fill the three-slot bank and the next reserve fails until
  `SendPackets` flushes, and `SendPackets` empties `claimed` while sending
  only `buffer`. Every withdrawal advances `send_staged`: the commit's
  caller, or `update`'s unconditional `send_staged++`, which the abort arm
  takes. `ClaimedFlushOwed` checks that the flush freeing a claimed slot is
  always owed. `AbortDisarmsSlot = FALSE` leaves the withdrawn frame armed
  in the staging: `MCRNDriverSendStagingBrokenAbortLeavesArmed.cfg` must
  fail `WithdrawnNeverSent`, the next flush handing the driver an empty
  frame or, on the simulator arm, a datagram the simulator already sent
  (TLC's trace below). The simulator choice is drawn at `Commit`; the
  disconnect burst and maintenance commit through the same helper and are
  not given a second arm.
- **The disconnect burst.** `disconnect_client` runs out of band, between
  `update` calls. It seals its burst through the same seam. With the rule,
  the burst advances the staged counter, so the next `send_packets` flushes
  it. Without the rule the counters still agree at that `send_packets` and
  the burst stays in staging until a later, unrelated commit advances the
  counter.

## Invariants

- **`StagedDisconnectFlushed`**: every frame committed by `disconnect_client`
  is handed to the driver's flush by the next `send_packets`. It is stated
  over the step after a `send_packets`: once `send_packets` has run, no
  disconnect frame is owed to the flush. It does not say the frame is on the
  wire: on XDP the driver's `send_packets` may retain its producer (a busy
  handoff lock, or no free replacement), and the frame leaves at a later
  flush under the XDP submodel's fairness assumption.
- **`WithdrawnNeverSent`**: a withdrawn frame is never handed to the driver.
- **`ClaimedFlushOwed`**: while a withdrawn frame's slot is claimed, the
  counters differ, so the next `send_packets` flushes and frees it.
- **`StagingWithinCapacity`**: armed slots, claimed slots and the
  outstanding reservation together never pass `MaxStaged`. TLC found the
  earlier model short here: a reservation did not count, so `Update` and
  `DisconnectClient` could fill the bank between a reserve and its commit,
  and the commit then took a fourth slot of three. The reservation now
  counts, as `rocketnet_server_reserve_send_frame` claims its slot at once.
- **`FlushedNeverAhead`**: the flushed counter never passes the staged
  counter.
- **`DiscOwedTracked`**: a disconnect frame owed is always still buffered or
  already sent; it is never lost from the accounting.
- **`TypeOK`**: every variable has its declared type.

## Liveness

- **`StagedFramesEventuallySent`**: every frame that reaches the send buffer
  is eventually sent, checked under weak fairness of `send_packets`
  (`SpecFair`). A frame that never commits — its reserve failed on a full
  staging — is dropped, not staged, and the property does not cover it.

## What the model leaves out

- Frames are abstract wire packets with a bounded life; the byte layout of a
  frame and the burst's packet contents are not modelled.
- One reservation is outstanding at a time, because every commit through the
  seam runs on the main thread. Payload fills from game threads reach the
  same staging by the driver's own start/finish pair and are represented by
  the seam and by `update`'s maintenance commit.
- `update`'s unconditional counter bump is taken by its seal and abort arms;
  on an update that touches no frame it is a bare generation tick (one more
  flush, nothing stranded), which keeps the counters bounded. Drivers that
  flush inside `update` are the `send_packets` case with the flush one call
  earlier.

## How to run it

On a Linux bench with TLC 2.19 or later, from `proofs/tla/`:

```sh
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 2 -cleanup -config MCRNDriverSendStaging.cfg MCRNDriverSendStaging
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 2 -cleanup -config MCRNDriverSendStagingBrokenDisconnect.cfg MCRNDriverSendStaging
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 2 -cleanup -config MCRNDriverSendStagingBrokenRefusesSend.cfg MCRNDriverSendStaging
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 2 -cleanup -config MCRNDriverSendStagingBrokenAbortLeavesArmed.cfg MCRNDriverSendStaging
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 2 -cleanup -config MCRNDriverSendStagingBrokenLockAlwaysBusy.cfg MCRNDriverSendStaging
```

The small instance:

| Constant | Value |
|---|---|
| `MaxFrames` | 4 |
| `MaxStaged` | 3 |
| `MaxBurst` | 2 |
| `DisconnectAdvancesStaged` | `TRUE` (`FALSE` in the witness) |
| `XdpBuffers` | 3 |
| `FinishRecordsRefusal` | `TRUE` (`FALSE` in its witness) |
| `AbortDisarmsSlot` | `TRUE` (`FALSE` in its witness) |

The historical transport-only run generated 7,037 states and found 1,198 distinct states,
with no error: the invariants hold and `StagedFramesEventuallySent` holds
under weak fairness of `send_packets`.

## The reversed witness

`MCRNDriverSendStagingBrokenDisconnect.cfg` is the committed cfg with
`DisconnectAdvancesStaged = FALSE`: `disconnect_client` seals its burst into
the send buffer and calls `send_packets` without advancing the staged counter
or flushing the send buffer. TLC must reject it, and does:

- **`Invariant StagedDisconnectFlushed is violated`**, at depth 3.
  - **The trace:**
    1. A frame is reserved through the seam (frame 1 outstanding).
    2. `disconnect_client` seals frame 2 into the send buffer and runs
       `send_packets`. The staged and flushed counters are both 0, so
       nothing flushes.
    3. Frame 2 is still owed after `send_packets` has run
       (`discOwed = {2}`, `sent = {}`).
  - **Against the code, by hand:** the trace is the staged path at the base
    of this change. `rocketnet_server_transport_disconnect_client`
    (line 1812) seals each burst packet through
    `rocketnet_server_reserve_send_frame` (line 1833) and
    `rocketnet_server_commit_send_frame` (line 1838). On a staged driver,
    commit finishes the packet into the driver's staging and returns
    (line 304): it advances neither counter. The only advances of
    `send_staged` are `update`'s tick (line 1242) and the payload finish
    paths (lines 1372 and 1494), and `disconnect_client` calls neither
    `update` nor `send_packets`. The next
    `rocketnet_server_transport_send_packets` (line 1725) reaches the staged
    branch's test `send_staged != send_flushed` (line 1745) with the
    counters still equal, so the driver flush does not run and the burst
    stays in staging. It leaves only when a later `update` tick or payload
    commit advances the counter — an out-of-band disconnect followed by
    `send_packets` without an intervening `update`, or a disconnect as the
    last call before shutdown, strands the burst across that `send_packets`,
    which is what `StagedDisconnectFlushed` forbids. The check by hand holds:
    the counterexample is real at the base, and the rule the invariant names
    is the missing advance.
  - **Current citation:** `rocketnet_server_transport_disconnect_client`
    in `sdk/rocketnet_server_transport.cpp` now cites this model. The source
    locations and measurements in this historical trace describe its old base.

`MCRNDriverSendStagingBrokenAbortLeavesArmed.cfg` (`AbortDisarmsSlot =
FALSE`) fails **`Invariant WithdrawnNeverSent`** at depth 3, 158 distinct
states (aws-mut-3, 2026-11-13):

1. `Update`'s maintenance reserves frame 1, finds nothing due and aborts it;
   the abort leaves the slot armed (`buffer = {1}`, `withdrawn = {1}`) and
   update's `send_staged++` makes the counters differ (`staged = 1`).
2. `send_packets` flushes: frame 1 is handed to the driver (`sent = {1}`).

Against the code by hand: `rocketnet_server_transport_update` calls
`rocketnet_server_abort_send_frame` when no keep-alive or ack is due, and
later `send_staged++` on a staged transport; the next
`rocketnet_server_transport_send_packets` sees the counters differ and
flushes the bank. What keeps the aborted slot off the wire is the driver's
abort zeroing its byte count (`packet_info[packet_index].packet_bytes = 0`
in `rocketnet_server_socket_abort_packet`, `slot->packet_bytes = 0` in
`rocketnet_uring_send_queues_abort`); without it the flush would send the
slot with whatever byte count it held. The passing model with the slot claimed: 105,288
distinct states in 23.7 s.

## Runs

The original two rows were in `proofs/tla/RUNS.tsv` from the runs on a Linux bench (TLC 2.19,
`-workers 2`, 2026-10-14): the committed run passes at depth 7, and the
reversed witness fails as expected at depth 3, with 21 states generated and
19 distinct states found when TLC stopped.
