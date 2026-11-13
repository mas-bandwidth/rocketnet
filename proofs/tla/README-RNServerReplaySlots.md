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
| `rec[i].skip`, `rec[i].skipAt` | `slots[i].skip_marked`, `slots[i].skip_address`: the address the slot's mark was queued at while the slot was skipped |
| `supOwed` | `marks->supersede_owed`: the last walk left a refused forget |
| `kernel[a]` | the kernel replay map's entry for `a` |

The ghost `owed` holds each `<<slot, address>>` the walk saw the slot
connected at, or queued the skipped slot's mark at, until the sink accepts a
forget of that address from that slot, or another slot's queued forget or mark
of the address supersedes it.

## Actions and the code they mirror

- **`Connect(i, a)`**, **`Open(i)`**, **`Release(i)`**: the environment. A
  client takes a free slot at an address no connected slot holds, with
  `highest_opened` at 0; a genuine packet raises `highest_opened`; the slot is
  released.
- **`Walk`**: `rocketnet_server_replay_marks_walk`, then the transport's
  commit. The forget pass asks the sink to forget `seen_address` for every seen
  slot that is no longer connected or is connected at another address, and
  `skip_address` for every slot with a skip mark that is no longer connected
  at it; only an accepted forget clears what it named
  (`rocketnet_server_replay_marks_clear_record`, `_clear_skip`), and it clears
  every other slot's record or skip mark of the same address
  (`rocketnet_server_replay_marks_supersede`, `ForgetSupersedes`). The code scans for such a record only while
  `supersede_owed` is set (`supOwed`): the walk sets it at its end when it
  left a refused forget, and clears it otherwise. Two records name one address
  only after such a walk (`DistinctUnlessOwed`), so every other walk's queued
  forgets have nothing to clear, and a mass disconnect reads each slot once
  instead of every slot for every forget. The mark pass, for every connected slot, skips a slot
  whose record still names another address, keeping the record, but asks the
  sink to mark the slot's new address at its `highest_opened` all the same
  (`MarkWhileSkipped`): the kernel drops a session packet from a source with
  no entry, so a session greeted with no entry would lose every packet until
  the old forget lands. A refused zero mark of a skipped slot holds the
  session's greeting, as any refused zero mark does (`Open` needs `greeted`:
  a client sends no session packet before its first keep-alive, so an
  ungreeted slot's mark is always zero). `Held` holds the greeting of each
  session whose mark was refused; the code is coarser, and holds every
  keep-alive of the next publish when any zero mark was refused, so it
  greets no session the model would hold. A mark queued in the skip is
  recorded in the slot's skip mark (`RecordSkip`): the record keeps the old
  address, and the forget pass forgets the skip address once the slot has
  left it, so a session that ends before the old forget lands leaves no
  kernel entry behind (`KernelEntryOwned`). A skipped slot whose skip mark
  still names an address it has left (that forget refused too) is not marked
  at all, so the skip mark keeps its owner (`KeepSkipOwner`). Otherwise the
  walk sets `seen` and `seen_address`, clearing a skip mark of the same
  address, which is the record's now, and asks the sink to mark a slot first
  seen, owing a mark, or whose `highest_opened` differs from its last mark; a
  refused mark sets `mark_owed`, and an accepted one clears every other
  slot's record or skip mark of the address (`MarkSupersedes`). The code looks for such records only when a
  forget of the same walk was refused: after the forget pass, a record names an
  address its slot is not at only when that slot's forget was refused. The
  commit applies every queued forget, then every queued mark.

The sink's answers are chosen freely: with `ForgetRefusable` a forget may be
refused, and a mark may always be refused
(`rocketnet_xdp_replay_marks_forget` and `_mark` refuse when their array is
full). The kernel's own refusals belong to
[RNXdpReplayMarksLayout](README-RNXdpReplayMarksLayout.md) and RNXdpReplay;
here the commit applies everything.

The XDP sink adds one rule of its own (`ZeroMarkWaitsForForgets`,
`rocketnet_xdp_replay_sink_mark`): it refuses a zero mark while its forget
array is full, and holds every greeting. A forget refused in a walk means the
array is full for the rest of that walk, so a skipped slot's mark, which the
walk asks at zero whatever the session opened before its greeting, is
refused: with the XDP sink no skip mark is ever recorded, and the session's
greeting waits for the old forget. A zero mark that replaces a kept mark of
its key in place is not refused (RNXdpReplay's `QueueRefusable`). The rule
covers every new session, not only skipped slots: while kernel deletes keep
failing and the forget array stays full, no new session is greeted.
msgx (`sdk/rocketnet_driver_builtin.cpp`, macOS) is a sink without the rule:
there a skip mark can be recorded, and a slot that keeps its skip mark's
owner waits unmarked for its forgets. The
skip mark keeps every entry owned with any sink (the default,
`ZeroMarkWaitsForForgets = FALSE`, whose answers are free), and the rule keeps
every greeting entered with the XDP sink. Without the rule, at three
addresses, a slot whose skip mark keeps its owner is not marked and its
session is greeted with no entry (`MCRNServerReplaySlotsBrokenZeroMarkTaken.cfg`).

The mark pass is one step here: `Cand` and `Blocked` are taken from the
records after the forget pass, before any of the mark pass's supersedes. The
code decides slot by slot, after the supersedes of the slots before it, so a
skip mark an earlier slot's mark cleared leaves its slot marked in the code
and Blocked in the model for that walk. Both keep every entry owned. No
trace check binds this model to the code yet; the xdp-replay trace check
(`tests/tla_trace_xdp_replay.cpp`) leaves slot reuse out. Binding it is on
`docs/roadmap.sexp`.

## Invariants

- **`ForgetNeverLost`**: every owed forget's slot still names that address in
  its record or its skip mark, so the walk asks for it again.
- **`OneIdentityPerSlot`**: a slot owes forgets for two addresses at most, its
  record's and its skip mark's.
- **`KernelEntryOwned`**: every kernel entry's address is named by some slot's
  record or skip mark, whose forget deletes it once the slot leaves. An entry
  with no owner would stay in the map until the transport is recreated.
- **`KernelHoldsMark`**: a connected slot whose record holds its current mark,
  owing nothing, has that mark in the kernel map.
- **`DistinctUnlessOwed`**: unless `supOwed` is set, no two slots' records
  or skip marks name one address. This is what the forget pass's skipped
  scan rests on.
- **`GreetedSlotHasEntry`**: a connected slot whose session has been greeted
  (`greeted[i]`, a ghost: the walk's commit released its held greeting
  because no mark the session needs was refused) has a kernel entry at its
  address. The kernel drops a session packet from a source with no entry
  (`XDP_KERNEL_DROPPED_NO_SESSION`), so this is what keeps a greeted
  session's traffic flowing. The one exception is a slot whose own record
  owes its mark: another slot's stale record of the address had its forget
  queued, the forget's supersede cleared this slot's record, and the sink
  refused the re-mark the same walk asked for (the mark buffer or kernel map
  full, an error path). The commit deletes the entry, and the session's
  packets drop as no-session until a later walk's mark lands; before the
  kernel dropped for no entry they passed without the kernel's replay check.
  TLC found this window in the control (`MCRNServerReplaySlots.cfg`) before
  the exception was written; it is accepted, as the disconnect burst of
  [RNXdpReplay](README-RNXdpReplay.md) is.

## The instance

Two client slots, two addresses, `highest_opened` 0..1.
`MCRNServerReplaySlots.cfg` is the control with forgets and marks refusable,
every invariant. `MCRNServerReplaySlotsNoForgetRefusal.cfg` is the control with
every forget queued, every invariant. In both, the forget pass's scan is
guarded by `supOwed` and the mark pass's by a forget refused in the same walk,
as in the code. `MCRNServerReplaySlotsScanEveryForget.cfg`
(`ScanWhenOwedOnly = FALSE`) is the control of the v1.0.0 walk, whose forget
pass scanned for every queued forget: every invariant, and its record in
`RUNS.tsv` counts the same states as `MCRNServerReplaySlots.cfg`, since the
scan the code skips clears nothing.

A slot reused twice while both its forgets are refused needs three addresses
(`MCRNServerReplaySlotsThree.tla`, two slots, `highest_opened` 0: no session
opens, which the reuse chain does not need, and at 0..1 the instance does
not finish inside the 110 s budget). `MCRNServerReplaySlotsThree.cfg` is the
control with any sink: every invariant but `GreetedSlotHasEntry`.
`MCRNServerReplaySlotsThreeXdpSink.cfg` adds the XDP sink's rule: every
invariant.

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
6. **`MCRNServerReplaySlotsBrokenNoOweOnRefusal.cfg`**
   (`OweOnRefusal = FALSE`): the walk without
   `marks->supersede_owed = refused_forgets > 0;`, so the forget pass never
   scans. Violates `KernelHoldsMark` by the sequence of witness 5: the walk
   that refused slot 0's forget and slot 1's mark leaves both records naming
   `a1` and owes nothing, and the next walk's queued forget of `a1` leaves
   slot 0's record with a mark the commit deletes.
   `test_server_replay_marks_refused_forget_owes_the_next_scan` drives the
   sequence and counts the records each walk's scans read (none, none, both
   slots, none); `test_server_replay_marks_mass_disconnect_is_linear` holds
   the cost (no record read at `ROCKETNET_MAX_CLIENTS` disconnects, where
   scanning for every forget reads 1,000,000 at 1000 clients).
7. **`MCRNServerReplaySlotsBrokenSkipUnmarked.cfg`**
   (`MarkWhileSkipped = FALSE`): the mark pass's skip as it was, a bare
   `continue`. Violates `GreetedSlotHasEntry`: slot 0 is seen at `a1` and
   released; slot 0 is taken again at `a2` with `highest_opened` 0. The walk's
   forget of `a1` is refused, the mark pass skips slot 0, no mark of `a2` is
   asked for and so none is refused, and the commit releases the greeting with
   no entry at `a2`: every session packet from `a2` is dropped as no-session
   until a later walk lands the forget. In the C: `forget` returns false,
   `continue` passes `rocketnet_xdp_replay_sink_mark`, which is where a refused
   zero mark sets the greeting hold. With the fix the skip calls
   `sink->mark( address, highest_opened )` and the hold follows from its
   answer. `test_server_replay_marks_reused_slot_with_owed_forget_marks_at_zero`
   drives the sequence through the sink.
8. **`MCRNServerReplaySlotsBrokenSkipNotRecorded.cfg`**
   (`RecordSkip = FALSE`): the skip's accepted mark not recorded, as in
   `9ac8aab4b`. Violates `KernelEntryOwned`: slot 0 is seen at `a1` and
   released, then taken again at `a2`; the forget of `a1` is refused and the
   mark of `a2` queued and applied, and from that commit no record or skip
   mark names `a2` (TLC stops there). In the C, slot 0 released before the
   forget of `a1` lands makes the next walk forget `a1` only, and `a2`'s
   entry stays. `test_server_replay_marks_skipped_mark_is_forgotten_when_its_session_ends`
   drives the sequence through the sink, red before the fix.
9. **`MCRNServerReplaySlotsBrokenSkipOverwritten.cfg`**
   (`KeepSkipOwner = FALSE`, three addresses): a slot whose skip mark still
   names an address it has left is marked at its new address and the skip
   mark overwritten. Violates `KernelEntryOwned`: slot 0 is seen at `a1`,
   then at `a2` with the forget of `a1` refused and `a2` marked in the skip,
   then at `a3` with both forgets refused; marking `a3` replaces the skip
   mark of `a2`, whose entry loses its owner.
   `test_server_replay_marks_blocked_slot_keeps_its_skip_mark` drives the
   sequence through the sink, red before the fix.
10. **`MCRNServerReplaySlotsBrokenZeroMarkTaken.cfg`** (any sink, three
    addresses, checking `GreetedSlotHasEntry`): the sequence of witness 9
    with `KeepSkipOwner`: slot 0 at `a3` is not marked, nothing is refused
    for it, and its session is greeted with no entry. With the XDP sink the
    skip mark of `a2` is refused at zero in the first place.
    `rocketnet_server_socket_test_replay_marks` (`tests/test_xdp.cpp`)
    checks the rule on the socket: a zero mark refused while the forget
    array is full, every keep-alive held, a nonzero mark still queued; red
    before the fix.

## Runs

The records are in `RUNS.tsv` (group `rnserverreplayslots`), made with the
pinned runner as `proofs/tla/README.md` describes.
