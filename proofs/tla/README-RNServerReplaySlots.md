# RNServerReplaySlots

The per-slot records of the server's kernel replay walk, as kept in
`sdk/rocketnet_server_replay_marks.cpp`. [RNXdpReplay](README-RNXdpReplay.md)
keys its walk by address. The code keys it by client slot: a slot can be
reused at another address while the forget of its old one is still owed, and
an address can move to another slot. This model carries those records and that
reuse.

## State

| Model | Code |
|---|---|
| `conn[i]`, `[on, at, high]` | client slot `i`: `rocketnet_client_slots_connected`, its mapping's address, its session's `replay_session.highest_opened` |
| `rec[i].seen`, `rec[i].at` | `slots[i].seen`, `slots[i].seen_address` |
| `rec[i].has`, `rec[i].marked` | `slots[i].has_mark`, `slots[i].marked_value` |
| `rec[i].owes` | `slots[i].mark_owed` |
| `kernel[a]` | the kernel replay map's entry for `a` |

The ghost `owed` holds each `<<slot, address>>` the walk saw the slot
connected at, until the sink accepts a forget of that address from that slot,
or another slot's queued forget or mark of the address supersedes it.

## Actions and the code they mirror

- **`Connect(i, a)`**, **`Open(i)`**, **`Release(i)`**: the environment. A
  client takes a free slot at an address no connected slot holds, with
  `highest_opened` at 0; a genuine packet raises `highest_opened`; the slot is
  released.
- **`Walk`**: `rocketnet_server_replay_marks_walk`, then the transport's
  commit. The forget pass asks the sink to forget `seen_address` for every seen
  slot that is no longer connected or is connected at another address; only an
  accepted forget clears the record (the `memset`), and it clears every other
  slot's record of the same address (`rocketnet_server_replay_marks_supersede`,
  `ForgetSupersedes`). The mark pass, for every connected slot, skips a slot
  whose record still names another address; otherwise it sets `seen` and
  `seen_address`, and asks the sink to mark a slot first seen, owing a mark,
  or whose `highest_opened` differs from its last mark; a refused mark sets
  `mark_owed`, and an accepted one clears every other slot's record of the
  address (`MarkSupersedes`). The code looks for such records only when a
  forget of the same walk was refused: after the forget pass, a record names an
  address its slot is not at only when that slot's forget was refused. The
  commit applies every queued forget, then every queued mark.

The sink's answers are chosen freely: with `ForgetRefusable` a forget may be
refused, and a mark may always be refused
(`rocketnet_xdp_replay_marks_forget` and `_mark` refuse when their array is
full). The kernel's own refusals belong to
[RNXdpReplayMarksLayout](README-RNXdpReplayMarksLayout.md) and RNXdpReplay;
here the commit applies everything.

## Invariants

- **`ForgetNeverLost`**: every owed forget's slot still names that address in
  its record, so the walk asks for it again.
- **`OneIdentityPerSlot`**: a slot owes forgets for one address at a time.
- **`KernelHoldsMark`**: a connected slot whose record holds its current mark,
  owing nothing, has that mark in the kernel map.

## The instance

Two client slots, two addresses, `highest_opened` 0..1.
`MCRNServerReplaySlots.cfg` is the control with forgets and marks refusable,
every invariant. `MCRNServerReplaySlotsNoForgetRefusal.cfg` is the control with
every forget queued, every invariant.

## The stale forget

Before the supersede rules the walk let a forget the sink refused delete the
kernel entry of a session in another slot.
`MCRNServerReplaySlotsBrokenStaleForget.cfg` keeps that walk and fails
`KernelHoldsMark`. Checked against the C by hand:

1. Slot 0 connects at `a1` and the walk sees it there.
2. Slot 0 is released; slot 1 connects at `a1`.
3. The walk's forget pass asks to forget `a1` for slot 0; the sink refuses
   (the forget array is full), so slot 0's record keeps `a1`. The mark pass
   sees slot 1 first and marks `a1` at 0; the commit writes the entry.
4. The next walk's forget of `a1` for slot 0 is accepted and the commit
   deletes `a1`'s entry. Slot 1's record says its mark of 0 is in place and
   `highest_opened` is unchanged, so no mark is taken.

Each retry of the forget also called `rocketnet_xdp_reply_macs_forget` for
`a1` (`rocketnet_server_socket_replay_forget`), turning slot 1's learned reply
MAC into an answered one. With `MarkSupersedes`, slot 1's queued mark in step 3
clears slot 0's record, so the forget is never asked for again.
`test_server_replay_marks_queued_mark_supersedes_an_owed_forget`
(`tests/test_unit_server_replay_marks.cpp`) drives this sequence through the
sink and a simulated commit.

The forget array is refused only at `ROCKETNET_XDP_REPLAY_MAX_ENTRIES` (1024 at
1000 clients), and one walk adds at most one forget per slot, so the refusal
needs forgets the kernel refused earlier still pending: an error path. The
userspace replay window refuses replays whatever the kernel entry holds.

## Reversed witnesses

Each turns one rule off and must fail with the invariant it names. Every
counterexample below was checked against the C by hand.

1. **`MCRNServerReplaySlotsBrokenNoSkip.cfg`** (`SkipWhileOwed = FALSE`): the
   mark pass without `if ( slot->seen && !rocketnet_address_equal( address,
   &slot->seen_address ) ) continue;`. Violates `ForgetNeverLost`: slot 0 is
   seen at `a1`, released and reconnected at `a2`; the forget of `a1` is
   refused, and the mark pass sets `seen_address` to `a2`, so `a1` is never
   forgotten.
2. **`MCRNServerReplaySlotsBrokenClearOnRefusal.cfg`**
   (`ClearOnAccept = FALSE`): the forget pass clearing the record whatever
   `sink->forget` returned. Violates `ForgetNeverLost`: a slot is seen at
   `a1` and released; the forget is refused and the record cleared, so no
   walk asks for it again.
3. **`MCRNServerReplaySlotsBrokenStaleForget.cfg`** (`MarkSupersedes = FALSE`,
   `ForgetSupersedes = FALSE`): the walk before the supersede rules. Violates
   `KernelHoldsMark` by the sequence above.
4. **`MCRNServerReplaySlotsBrokenNoMarkSupersede.cfg`**
   (`MarkSupersedes = FALSE`): the mark pass without the supersede after an
   accepted mark. Violates `KernelHoldsMark`: slot 0 marks `a1` at 1 and is
   released; its forget is refused while slot 1 connects at `a1` and marks it
   at 0. Slot 1 is released and slot 0 connects at `a1` again; its record still
   holds its mark of 1, which the kernel no longer has.
5. **`MCRNServerReplaySlotsBrokenNoForgetSupersede.cfg`**
   (`ForgetSupersedes = FALSE`): the forget pass without the supersede after
   an accepted forget. Violates `KernelHoldsMark`: slot 0 marks `a1` at 0 and
   is released; its forget is refused while slot 1 connects at `a1`, and slot
   1's mark is refused. Slot 1 is released and slot 0 connects at `a1` again
   with its record holding its mark of 0; the walk queues slot 1's forget of
   `a1`, the commit deletes the entry, and slot 0 is not marked.
   `test_server_replay_marks_queued_forget_supersedes_a_held_mark` drives
   this sequence through the sink.

## Runs

The records are in `RUNS.tsv` (group `rnserverreplayslots`), made with the
pinned runner as `proofs/tla/README.md` describes.
