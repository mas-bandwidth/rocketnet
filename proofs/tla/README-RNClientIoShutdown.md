# Shutdown request and completion ownership

`RNClientIoShutdown.tla` is an additive finite ownership abstraction for the
2026-10-21 API decision. It is **not a proved refinement or composition** of the
existing client-I/O, receive-quit, or public-session models. Their earlier
measurements remain evidence for their exact old inputs; their synchronous
shutdown assumptions do not describe this API.

The public client calls disconnect/disconnected; the public server and both
transports call stop/stopped. Each request disables new work, retains the
object, and returns without waiting for worker progress. It does no worker's
work, with one backend call on the client I/O path: a development link
simulator belongs to the caller there, never to the I/O thread, so
`rocketnet_client_transport_stop` sends every copy it still holds
(`rocketnet_client_link_drain` at `DBL_MAX`) before it hands over the final
ring. Those are synchronous sends that wait on no worker; they are
`RNClientLinkStop`'s `Stop` on its `iocore` path, and this model, which has no
packets, has no step for them. A completion check
returns false until all worker ownership has ended. Destroy is synchronous,
requires completion, joins threads, then frees. Restart/reconnect may finalize
the previous generation synchronously after completion; storage reuse requires
its old joins. A freshly created endpoint has no worker and is already complete.
The source corresponds to combined candidate `2d043ab7bd6748eb17c9cbb47415c6a3fa944e47`;
exact final implementation correspondence still requires source review.

## Actions and ownership

The four named control families instantiate one module:

- `RNClientIo`: one worker, three committed send rings
  (`ROCKETNET_CLIENT_IO_NUM_RINGS`) and worker-owned keys. Start seeds the
  difficult boundary: ring 0 is half-sent (Sending), ring 1 is ready in the
  queue (HandedOff), and ring 2 is Filling. Request publishes ring 2 as
  HandedOff without waiting for ring 0 or ring 1, changing their ownership,
  reopening a ring or making producer space. The worker finishes all three
  rings, wipes its key copy, then acknowledges quiescence. The endpoint also
  owns one socket (`sock`, `queued`): `Arrive` queues a datagram on the open
  socket, and the worker's quiescence drains it in counted reads, then
  closes it (`rocketnet_client_io_discard_at_stop`,
  sdk/rocketnet_client_io.h), so a later datagram meets a closed port. The
  drain is bounded (below). The ghost `lost` records a queue that `Free` or
  the next generation's `Start` frees unread; `closeLoss` records datagrams
  the close took unread.
- `RNClientReceiveQuit`: one receive worker, no send-ring abstraction.
- `ClientSession`: one retained transport owner and its key storage. This is
  lifecycle coverage, not another handshake or wire-state model.
- `ServerSession`: two independently completing worker owners. Completion is
  their conjunction, so one stuck owner keeps the server pending.

The worker's acknowledgement precedes `WorkerExit`, which represents the OS
thread epilogue. A query may be true before that epilogue ends; synchronous
finalization's `Join` waits for Exit, then Free is safe. The final join and
allocator/native cleanup have no modeled elapsed-time bound.

`RequestStop` publishes shutdown and invokes the modeled terminal callback
once. `RequestReturn` depends only on caller execution. `BeginPoll` and
`PollReturn` observe completion without joins or cleanup. `RepeatStop` is
idempotent. `BeginRestart`/`Start` enforce previous-generation completion and
joins; `DestroyBegin`/`Join`/`Free` enforce completion before reclamation.

Every positive family has a progressing control and a permanently stuck-worker
control. In the latter, the selected worker cannot finish or acknowledge, while
request and poll return. It checks `StuckRemainsPending` and deliberately does
not claim `StopCompletes`. Fairness concerns caller return/poll actions and
only enabled worker actions; it never enables the permanently stuck worker.
`DestroyCompletes` is conditional on the application initiating valid destroy;
the model does not require the application to destroy an object.

## Invariants and negative controls

Positive controls check type consistency, true completion implies quiescence
and key wipe, no free before joins, retained context, no premature reuse,
exactly-once terminal callback, never-started completion, and immediate enabled
request/poll returns. `NeverStartedComplete` and `PollNeverWaits` describe
modeled definitions/guards; they are bookkeeping checks, not independently
reversed guarantees. Negative controls separately restore:

1. Waiting for quiescence inside request.
2. Treating the request flag as completion.
3. Freeing before completion/join.
4. Reusing a generation before completion.
5. Acknowledging before wiping worker-owned keys.
6. Calling the terminal callback again on repeated request.
7. Exiting with the socket's queue unread and the socket open
   (`exitunread`): `StoppedSocketDrained` fails, a completed stop with the
   socket still open and a datagram queued on it. The positive `RNClientIo` controls also check
   `StoppedSocketDrained` and `NoUnreadDiscard`.
8. Closing the wake handle at the worker's exit before the stop request's
   wake call has returned (`closebeforewake`): `NoWakeAfterClose` fails, the
   request's wake writes to a closed, possibly reused, descriptor. The code
   stores `stop_woken` after the wake call and the exit waits for it before
   `close` (sdk/rocketnet_client_io.h). Found by ThreadSanitizer in
   test_lifecycle.

## The exit's bounded drain

The client I/O worker's `WorkerQuiesce` is three kinds of step, as
`rocketnet_client_io_discard_at_stop` runs them: reads, each taking one queued
datagram and counting it in `receive_discarded_at_stop`; the end of the drain;
and the close. The drain ends at an empty read, at a read the transport
refuses (counted as a transport fault, which can come at any read), or after
`ROCKETNET_CLIENT_IO_DISCARD_MAX` datagrams (`DiscardCap`, 2 here, against a
one-slot queue, so a sender that refills the queue at every read reaches it).
The exit then waits for `stop_woken` and closes the socket. A datagram that
arrives after the drain's last read, or past the cap, is still queued at the
close and goes with it, unread and uncounted: the model allows it (`closeLoss`)
because the code does. A bounded drain is the design (a flooding sender
cannot hold the exit); the claim is what changed. `StoppedSocketDrained` says
the socket a completed stop reports is closed with nothing waiting on it, not
that every datagram was read; `NoUnreadDiscard` says no queue waits unread for
`Free` or the next `Start`. `MCRNClientIoShutdownReachCloseDrops.cfg` is a
reach case, not a property of the code: it breaks `NoCloseLoss` (TLC prints a
behaviour in which a datagram queued after the drain ended goes with the
close).

The three-ring fixture intentionally tests the final handoff boundary; ordinary
ring allocation, payload bytes, address binding and receive publication remain
in `RNClientIo`. That old model's blocking DestroyWait/Timeout path is
historical, not silently accepted as the new shutdown algorithm.

## Limits and evidence

This abstraction assumes caller serialization and joined producers before stop.
It does not prove arbitrary user callbacks return, atomic implementation memory
orders, OS thread latency, wake syscalls, native cancellation, RIO/io_uring
completion accounting, partial-create cleanup, or transport-specific locks.
The stalled-owner controls cover ownership safety when progress never arrives;
they are not timing tests. Platform regressions and exact implementation review
remain required.

All new cases are in group `shutdownownership`; their module/config dependency
fingerprints and measured results must be generated by `tlacheck` on an allocated
Linux bench. Existing model fingerprints and historical receipts are preserved.
