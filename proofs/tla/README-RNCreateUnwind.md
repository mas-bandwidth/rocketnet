# RNCreateUnwind

`RNCreateUnwind.tla` models who owns a transport whose create fails after
its I/O worker started. The worker still reads the transport until it exits,
so the transport cannot be freed before then, and a create that fails returns
no handle. The model is the ownership only: no packets, no slots, no keys.

## Abstraction

- **Build, Succeed.** The create holds the transport from the moment it
  exists; on success it returns the handle and the caller owns it.
- **FailInTransport.** The server transport's own create fails after the
  worker started (`rocketnet_server_transport_abort_create`,
  `sdk/rocketnet_server_transport.cpp`): the transport is told to stop.
- **FailInMessage.** The message object's create fails after its transport
  was created (`rn_message_link_abort_create`,
  `sdk/rocketnet_message_server.cpp`), for example a layered
  `rocketnet_message_server_create` or `rocketnet_message_client_create`
  with no caller block whose one allocation is refused.
- **WorkerExits.** The environment: the worker exits at some point after it
  was told to stop. The invariants hold on every prefix, so they cover a
  worker that is still running at any length; the liveness rules assume it
  exits eventually.
- **UnwindWaits.** One poll of the unwind's wait. `Budget` polls stand for
  the one-second deadline. The wait has the deadline only where a retained
  slot can take the transport: the server start's `failed_start`
  (`rocketnet_server_start`, `sdk/rocketnet_api.cpp`). Every other unwind
  waits with no deadline, as the message create did before the deadline was
  added; it blocks for as long as the worker runs.
- **UnwindFrees.** The worker has exited: the transport is destroyed and the
  create returns NULL.
- **UnwindDeadline.** A bounded wait ran out with the worker still running:
  the transport goes to the retained slot.
- **RetainedFrees.** The next start or the destroy finds the retained
  worker exited and frees the transport (`rn_server_finish_stop`).

## Properties

- `TypeOK` holds the ranges.
- `LiveTransportOwned`: a transport that was created and not freed has an
  owner: the create, the caller's handle or the retained slot.
- `FreedOnlyAfterExit`: a transport is freed only once its worker has
  exited, never under it.
- `CreateOwnsOnlyWhileCreating`: the create holds it only while it runs.
- `UnwindEnds` (temporal): a failed create returns.
- `FailedCreateFreed` (temporal): once the worker exits, a failed create's
  transport is freed.

## Instances

`MCRNCreateUnwind.cfg` is the code: `Budget = 3`, both failure places, with
and without a retained slot. It passes every rule above.

The reversed witnesses each restore one defect, and each reverses a
different rule or place:

- `MCRNCreateUnwindBrokenBoundedUnwind.cfg` (`Defect = "boundedunwind"`):
  the message create's unwind has the deadline, as it did before this rule.
  Fails `LiveTransportOwned`. TLC's trace: Build, FailInMessage, three
  UnwindWaits, UnwindDeadline; the create has returned with the worker still
  stopping, the transport not freed and no owner.
- `MCRNCreateUnwindBrokenNoRetain.cfg` (`Defect = "noretain"`): the server
  transport's bounded wait drops the transport on its deadline even with a
  retained slot, as the server start did before `failed_start`. Fails
  `LiveTransportOwned`. TLC's trace: Build, FailInTransport with a retained
  slot, three UnwindWaits, UnwindDeadline; owner none, not freed.
- `MCRNCreateUnwindBrokenFreeAtDeadline.cfg` (`Defect = "freeatdeadline"`):
  a bounded wait gives up and destroys the transport under its running
  worker. Fails `FreedOnlyAfterExit`. TLC's trace: Build, FailInTransport
  with a retained slot, three UnwindWaits, UnwindDeadline; freed with the
  worker still stopping.

## Checked against the code by hand

- `rocketnet_server_transport_abort_create` breaks out to the deadline only
  when `retained` is not NULL, stores the transport in `*retained` there, and
  otherwise loops until the worker reports stopped, then destroys it:
  UnwindDeadline and UnwindWaits/UnwindFrees.
- `rn_message_link_abort_create` calls `rn_message_link_stop_and_wait` with
  `bounded` false and destroys the transport after it returns: UnwindWaits
  then UnwindFrees, with no deadline and no slot.
- `rn_server_finish_stop` destroys `failed_start` once
  `rocketnet_server_stopped` is true: RetainedFrees.
- Not modelled: `rn_message_object_destroy` called before the transport
  stopped, a counted misuse whose wait keeps its one-second budget
  (`docs/roadmap.sexp`, `message-destroy-misuse-owner`). The model is not
  bound to the code by a trace check.
