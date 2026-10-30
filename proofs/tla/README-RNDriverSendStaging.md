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
flush publishes retained work after retirement. `XdpStagedEventuallyPublished`
checks eventual publication under weak fairness of publication and retirement.
The transport and XDP submodels interleave independently; this is a bounded
abstraction of generation ownership, not a proof of the C++ memory model.

The control uses `PublishWith(TRUE)` and checks both new invariants and the
publication liveness property. All three cases run through `tlacheck run --group rndriversendstaging`; `tlacheck merge --keep proofs/tla/RUNS.tsv` writes the
measured records. The frame and burst instance sizes remain unchanged.

### Reversed no-refusal witness

`MCRNDriverSendStagingBrokenRefusesSend.cfg` selects `SpecBrokenRefuses`, whose
publish action is `PublishWith(FALSE)`, which publishes the producer itself
when no replacement is free. TLC must reject exactly `NoRefusedSend`: start and
publish repeat until every buffer is queued, the last publish leaves
`xdpFilling = FALSE`, and the next start sets `xdpRefused = TRUE`. No worker
retirement occurs in that trace. (With two buffers the trace was depth 6; the
three-buffer records and trace are owed: see RUNS.tsv.)

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

The refreshed control generates 109,743 states, with 13,178 distinct states;
the no-refusal witness generates 20,895 states, with 4,113 distinct states.
The existing disconnect witness still rejects `StagedDisconnectFlushed`.

### Equal-counter retry proof

The earlier exploratory no-retry TLC trace was removed from the committed
case set. The production retry remains checked directly against the code:
removing the
`driver == ROCKETNET_DRIVER_XDP` disjunct in
`rocketnet_server_transport_send_packets` leaves the retained generation
unpublished once `send_flushed` catches up to `send_staged`, until an
unrelated commit advances the staged counter. The unit check
`test_xdp_transport_send_staging_reflushes_without_commit` fails for that schedule.

## Transport model and historical disconnect witness

The staged send path of the server transport: the reserve/commit seam every
driver serves, the staged branch of `send_packets`, the maintenance half of
`update`, and the out-of-band `disconnect_client`. The code is
`sdk/rocketnet_server_transport.cpp` at the base of this change:
`rocketnet_server_reserve_send_frame` (line 260),
`rocketnet_server_commit_send_frame` (line 288),
`rocketnet_server_transport_update` (line 983),
`rocketnet_server_transport_send_packets` (line 1725, the staged branch's
counter test at line 1745), and
`rocketnet_server_transport_disconnect_client` (line 1812).

- **The seam.** Reserve takes a staging slot for one frame; commit seals the
  frame into the driver's own staging. A reserve that exhausts packet-slot capacity
  fails and the frame is dropped; XDP generation ownership is modelled separately.
- **The publish rule.** On a staged driver a sealed frame leaves only at a
  driver flush, and `send_packets` flushes exactly when the staged counter
  differs from the flushed counter. The model states the rule the path
  needs: every commit through the seam advances the staged counter.
- **The disconnect burst.** `disconnect_client` runs out of band, between
  `update` calls. It seals its burst through the same seam. With the rule,
  the burst advances the staged counter, so the next `send_packets` flushes
  it. Without the rule the counters still agree at that `send_packets` and
  the burst stays in staging until a later, unrelated commit advances the
  counter.

## Invariants

- **`StagedDisconnectFlushed`**: every frame committed by `disconnect_client`
  leaves by the next `send_packets`. It is stated over the step after a
  `send_packets`: once `send_packets` has run, no disconnect frame is owed.
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
- `update`'s unconditional counter bump is a bare generation tick: it stages
  no frame and strands none. Drivers that flush inside `update` are the
  `send_packets` case with the flush one call earlier.

## How to run it

On a Linux bench with TLC 2.19 or later, from `proofs/tla/`:

```sh
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 2 -cleanup -config MCRNDriverSendStaging.cfg MCRNDriverSendStaging
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 2 -cleanup -config MCRNDriverSendStagingBrokenDisconnect.cfg MCRNDriverSendStaging
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 2 -cleanup -config MCRNDriverSendStagingBrokenRefusesSend.cfg MCRNDriverSendStaging
```

The small instance:

| Constant | Value |
|---|---|
| `MaxFrames` | 4 |
| `MaxStaged` | 3 |
| `MaxBurst` | 2 |
| `DisconnectAdvancesStaged` | `TRUE` (`FALSE` in the witness) |

The committed run generates 7,037 states and finds 1,198 distinct states,
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
  - **The citation is owed, not made here.** No sdk file is edited by this
    change. The next change of `rocketnet_server_transport_disconnect_client`
    cites this model from that function.

## Runs

The original two rows were in `proofs/tla/RUNS.tsv` from the runs on a Linux bench (TLC 2.19,
`-workers 2`, 2026-10-14): the committed run passes at depth 7, and the
reversed witness fails as expected at depth 3, with 21 states generated and
19 distinct states found when TLC stopped.
