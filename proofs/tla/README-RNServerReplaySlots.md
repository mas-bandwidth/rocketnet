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
connected at, until the sink accepts a forget of that address from that slot.

## Actions and the code they mirror

- **`Connect(i, a)`**, **`Open(i)`**, **`Release(i)`**: the environment. A
  client takes a free slot at an address no connected slot holds, with
  `highest_opened` at 0; a genuine packet raises `highest_opened`; the slot is
  released.
- **`Walk`**: `rocketnet_server_replay_marks_walk`, then the transport's
  commit. The forget pass asks the sink to forget `seen_address` for every seen
  slot that is no longer connected or is connected at another address; only an
  accepted forget clears the record (the `memset`). The mark pass, for every
  connected slot, skips a slot whose record still names another address;
  otherwise it sets `seen` and `seen_address`, and asks the sink to mark a slot
  first seen, owing a mark, or whose `highest_opened` differs from its last
  mark; a refused mark sets `mark_owed`. The commit applies every queued
  forget, then every queued mark.

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
every invariant but `KernelHoldsMark`. `MCRNServerReplaySlotsNoForgetRefusal.cfg`
is the control with every forget queued, every invariant including
`KernelHoldsMark`.

## The finding

`MCRNServerReplaySlotsFindingStaleForget.cfg` is the code's rules with a forget
the sink refuses, and fails `KernelHoldsMark`. Checked against the C by hand:

1. Slot 0 connects at `a1` and the walk sees it there.
2. Slot 0 is released; slot 1 connects at `a1`.
3. The walk's forget pass asks to forget `a1` for slot 0; the sink refuses
   (the forget array is full), so slot 0's record keeps `a1`. The mark pass
   sees slot 1 first and marks `a1` at 0; the commit writes the entry.
4. The next walk's forget of `a1` for slot 0 is accepted and the commit
   deletes `a1`'s entry. Slot 1's record says its mark of 0 is in place and
   `highest_opened` is unchanged, so no mark is taken.

Slot 1's session at `a1` then has no kernel entry until its `highest_opened`
changes. The forget also calls `rocketnet_xdp_reply_macs_forget` for `a1`
(`rocketnet_server_socket_replay_forget`), at the refusal and at the retry,
turning slot 1's learned reply MAC into an answered one. The superseded test in
`rocketnet_xdp_replay_marks_reset_keeping` covers a forget and a newer mark
of its key in the same commit, not a forget retried by the walk after the mark
landed. RNXdpReplay cannot show this: keyed by address, it owes no forget for
an address that is connected again.

The forget array is refused only at `ROCKETNET_XDP_REPLAY_MAX_ENTRIES` (1024 at
1000 clients), and one walk adds at most one forget per slot, so the refusal
needs forgets the kernel refused earlier still pending: an error path. The
userspace replay window still refuses replays while the kernel entry is
missing. With every forget queued (`MCRNServerReplaySlotsNoForgetRefusal.cfg`)
`KernelHoldsMark` holds.

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

## Runs

The records are in `RUNS.tsv` (group `rnserverreplayslots`), made with the
pinned runner as `proofs/tla/README.md` describes.
