# RNDriverSendStaging

The staged send path of the server transport: the reserve/commit seam every
driver serves, the staged branch of `send_packets`, the maintenance half of
`update`, and the out-of-band `disconnect_client`. The code is in
`sdk/rocketnet_server_transport.cpp`, and each function cites the model
action it implements: `rocketnet_server_reserve_send_frame` (`Reserve`),
`rocketnet_server_commit_send_frame` (`Commit`),
`rocketnet_server_transport_update` (`Update`),
`rocketnet_server_transport_send_packets` (`SendPackets`, whose staged branch
flushes when `send_staged != send_flushed`) and
`rocketnet_server_transport_disconnect_client` (`DisconnectClient`).

- **The seam.** Reserve takes a staging slot for one frame; commit seals the
  frame into the driver's own staging. A reserve that finds the staging full
  fails and the frame is dropped.
- **The publish rule.** On a staged driver a sealed frame leaves only at a
  driver flush, and `send_packets` flushes exactly when the staged counter
  differs from the flushed counter. The model states the rule the path
  needs: every commit through the seam advances the staged counter.
- **The disconnect burst.** `disconnect_client` runs out of band, between
  `update` calls. It seals its burst through the same seam and then
  advances the staged counter once, so the next `send_packets` flushes the
  burst even with no `update` in between. Without that advance the counters
  would still agree at that `send_packets`, and the burst would stay in
  staging until a later, unrelated commit advanced the counter.

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

On Linux with TLC 2.19 or later, from `tla/`:

```sh
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 2 -cleanup -config MCRNDriverSendStaging.cfg MCRNDriverSendStaging
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC -workers 2 -cleanup -config MCRNDriverSendStagingBrokenDisconnect.cfg MCRNDriverSendStaging
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
the send buffer without advancing the staged counter, and the same step
stands for the next `send_packets` (`ranSend`), which does not flush the send
buffer. TLC must reject it, and does:

- **`Invariant StagedDisconnectFlushed is violated`**, at depth 3.
  - **The trace:**
    1. A frame is reserved through the seam (frame 1 outstanding).
    2. `disconnect_client` seals frame 2 into the send buffer, and the next
       `send_packets` runs. The staged and flushed counters are both 0, so
       nothing flushes.
    3. Frame 2 is still owed after `send_packets` has run
       (`discOwed = {2}`, `sent = {}`).
  - **Against the code, by hand:** `rocketnet_server_transport_disconnect_client`
    seals each burst packet through `rocketnet_server_reserve_send_frame` and
    `rocketnet_server_commit_send_frame`. On a staged driver, commit finishes
    the packet into the driver's staging and returns without touching either
    counter. The other advances of `send_staged` are `update`'s tick and the
    payload finish paths, and `disconnect_client` calls neither `update` nor
    `send_packets`. Without its own advance, the next
    `rocketnet_server_transport_send_packets` would reach the staged branch's
    test `send_staged != send_flushed` with the counters still equal, skip the
    driver flush, and strand the burst in staging: an out-of-band disconnect
    followed by `send_packets` with no `update` in between, or a disconnect as
    the last call before shutdown. That is what `StagedDisconnectFlushed`
    forbids. The code makes the advance: after committing at least one burst
    frame on a staged driver, `disconnect_client` increments `send_staged`,
    which is `DisconnectAdvancesStaged = TRUE`.

## Runs

Both rows are in `tla/RUNS.tsv` from runs on Linux (TLC 2.19,
`-workers 2`): the committed run passes at depth 7, and the
reversed witness fails as expected at depth 3, with 21 states generated and
19 distinct states found when TLC stopped.
