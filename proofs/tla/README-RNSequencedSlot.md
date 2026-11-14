# Sequenced block dispatch on the receiver's descriptor slots

`RNSequencedSlot.tla` models the receive dispatch of a reliable-sequenced
(code 4) block superseded by a later ordered epoch, on the code's descriptor
slots, against an honest sender bounded by its own window. Its invariant is
that such a sender never drives the receiver to `SLOT_COLLISION` (check 10).

## Workload and sender

One channel, in channel FIFO order: Y (number 0, reliable ordered, ordering
index 0); O (numbers 1 and 2, a two-fragment code-4 block, ordering 1,
sequencing 0); Z (3, reliable ordered, ordering index 1); N (4, a one-record
code-4 state, ordering 2, sequencing 0); then plain reliable records (code 2)
up to `MaxNum`. The stamps are the sender's: an ordered send takes
`ordered_write` and resets `sequenced_write`, a sequenced send takes both.
`Number` reserves a block's numbers at once and admits
`next + count <= U' + W`, U' the first number of the block holding the lowest
unacked number (`rn_window_send_admit`, `rn_window_send_first_write_block`).
Packets carry one number each and arrive in any order; a number becomes
ackable at the first `EndUpdate` after it was accepted, since a delivered view
lives until then.

## Receiver state and actions

`desc[s]`, for `s` in `0..W-1`, is `receive_descriptors[s]`: live, the number
it holds, `block_count` and `block_have`. A released descriptor keeps its
number, as `live = 0` does in the code. `ord`, `wm`, `hs`/`hsf` and
`bit`/`bitf` are `ordered_read`, `sequenced_watermark`, `held_sequenced` and
`held_sequenced_first`, `block_in_turn` and `block_in_turn_first`; `hord` is
the held index of ordered records ahead of their turn; `dl` the delivery
table's descriptor slots.

| Action or operator | Code |
|---|---|
| `Receive(n)` | `rn_window_receive_accept` (`sdk/rocketnet_window_receive.cpp`): a live descriptor for another number at `n % W` is `SLOT_COLLISION`; else the descriptor is claimed and R advances. Then the dispatch below. |
| `DispatchSeq`, `Future`, `Place` | `rn_channels_dispatch_sequenced` (`sdk/rocketnet_channel_sequenced.cpp`): stale behind the epoch, in turn, or the one future-state slot with supersede and continuation |
| `InTurn` | `rn_sequenced_in_turn`: the in-turn block's gap, the watermark, then the superseding release, a record or a block |
| `Block`, `Collect` | `rn_sequenced_block`: the accepted-span check, anchor or join, completion at the first slot, delivery in turn |
| `ReleaseSlot`, `ReleaseNumber`, `HoldValid`, `Owns` | `rn_sequenced_release_slot`, `rn_sequenced_release_number`, `rn_sequenced_hold_valid`, `rn_sequenced_block_owns` |
| `DrainSeq` | `rn_channels_drain_sequenced`: an in-turn block of a closed epoch released, a future hold promoted to in turn or delivered |
| `DispatchOrd`, `DrainLoop`, `DeliverOrdered` | `rn_channels_dispatch_ordered`, `rn_channels_drain`, `rn_channels_deliver_ordered` (`sdk/rocketnet_channel.cpp`) |
| `EndUpdate` | `rn_channels_end_update` with `rn_channels_release`: each delivery's number, or a block's every fragment |
| `Number`, `Ack(n)` | the sender's window and the transport's ACK |

`CheckAcceptedSpan` is the fix in `rn_sequenced_block`: a fragment that would
open a new block while a sibling number of its span is a code-4 descriptor
that was accepted and is no longer live (released when the block was
superseded) is stale. `FALSE` removes it.

`InTurn` tests the watermark before it releases a superseded in-turn block,
as `rn_sequenced_in_turn` does: a newer record refused as stale leaves the
block as it was (standard m6.3). The order is not observable in this model:
every record's sequencing is 0, so no in-turn record supersedes, and with
natural arithmetic the in-turn block is never behind the watermark
(`InTurnBlockFresh`). `RNSequencingWrap.tla` carries the 24-bit compare and
the reversed witness `MCRNSequencingWrapBrokenReleaseBeforeRefuse.cfg`.

## Invariants

| Invariant | Meaning |
|---|---|
| `NoSlotCollision` | no accepted number meets a descriptor live for another number |
| `NoViolation` | no violation of any kind is counted (`SLOT_COLLISION`, `BAD_BLOCK`, `PROTOCOL_VIOLATION`, `NUMBER_OUTSIDE_WINDOW`, `ORDERING_BEHIND`) |
| `InTurnBlockFresh` | the in-turn block's sequencing is at or past the watermark (the controls) |
| `TypeOK` | types |

## Cases

| cfg | Constants | Expected |
|---|---|---|
| `MCRNSequencedSlotW4.cfg` | `W = 4`, `MaxNum = 6`, the fix | pass |
| `MCRNSequencedSlotW8.cfg` | `W = 8`, `MaxNum = 10`, the fix; slots 0, 1 and 2 are reused | pass |
| `MCRNSequencedSlotBrokenSpan.cfg` | `W = 8`, `MaxNum = 10`, no accepted-span check | `NoSlotCollision` violated |

The reversed witness, checked against the code by hand: numbers 0 to 7 are
issued; O#2 arrives while `ordered_read = 0` and anchors a future block in
`held_sequenced` (slot 2); N#4 arrives, newer, and supersedes it,
`rn_sequenced_release_slot` freeing descriptor 2; Y#0 is delivered and opens
epoch 1 (N stays held, ordering 2); O#1 arrives in turn. With the check, its
sibling 2 is a released code-4 descriptor, so O#1 is stale. Without it, O#1
anchors an in-turn block at slot 1 that can never complete. The update ends,
0, 1 and 2 are acked (Z#3 still in flight, so U' = 3), the sender numbers 8
and 9, and 9 arrives at slot 1, still live for 1: `SLOT_COLLISION`. The other
fragment order (O#1 first, then N, Y, O#2, a collision at slot 2 with number
10) is the same defect.

Bench probes (not recorded cases) show the W = 8 control is not vacuous: a
number from 8 to 10 is accepted on a reused slot, and the state where both O
fragments were accepted and released, Y delivered, N held and Z not yet
received is reached. At W = 4 the defect is not reachable with an honest
sender: removing the check there leaves the same 3,120 distinct states with
no collision.

## What stays abstract

The 24-bit wire arithmetic (the numbers stay far below the half range;
`RNSequencingWrap.tla` carries it), the ring bytes and the block shape checks
(an honest sender passes them), delivery-table room (never short here),
code-1 holds, more than one channel, packet ids, several records per packet,
duplicates and retransmission (a packet arrives at most once). The workload is
fixed: the model checks every arrival order, update end and ACK of this one
composition, not arbitrary workloads.
