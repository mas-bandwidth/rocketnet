# RNUnreliableRingLayout

The endpoint's unreliable queue at the layout of `sdk/rocketnet_endpoint.cpp`,
with the enqueue tickets of `sdk/rocketnet_allocator.h`. `RNIndexRingArray.tla`
carries the same ring (array, head, count, positions modulo the ring, the
in-place full-ring pass) with tickets that never wrap; `RNIndexRing.tla` and
`RNChannelQueues.tla` carry the queue abstractly. This model adds what the
endpoint's ring has beyond them: tickets that wrap, drawn from a counter the
reliable enqueues share, the renumber after a wrap, the
eager head sync, and the walk.

## State

| Model | Code |
|---|---|
| `ticket[d]` | `send_descriptors[d].enqueue_ticket`, 0 when `d` is not queued (`rn_endpoint_unreliable_unlink`) |
| `next`, `wrapped` | `region->schedule_ticket_next`, `region->schedule_ticket_wrapped` |
| `arr`, `head`, `count` | `region->unreliable_ring`, `endpoint->unreliable_ring_head`, `endpoint->unreliable_ring_used` |
| `wbase`, `wspan`, `wstep` | `rn_unreliable_walk_t`'s `base`, `span`, `step` |
| `order` | ghost: the queued descriptors in push order |
| `worder`, `wseen` | ghost: `order` when the walk began, and what the walk returned |

An entry is live when its descriptor is queued under the entry's ticket
(`rn_unreliable_entry_live`); any other entry is a tombstone. Tickets run
`1..TicketMod-1` (`2^32 - 1` values in the code), skip 0 and wrap. A push and
the loops inside it run with nothing interleaved, so `Push` is one action, and
its loops read the array as they rewrite it, in place, as the code does.

## Actions and the code they mirror

Functions in `sdk/rocketnet_endpoint.cpp` unless stated.

- **`Push(d)`**: `rn_endpoint_finish` stamps `d`
  (`rn_descriptor_enqueue_stamp`, through `rn_region_schedule_ticket` in
  `sdk/rocketnet_allocator.h`: the ticket, then `next + 1`, 0 skipped and the
  wrap noted) and pushes it: `rn_unreliable_push` calls
  `rn_unreliable_renumber` when the wrap flag is set: the flag cleared, every
  entry naming `d` and every tombstone dropped, each survivor given the next
  ticket and written at `head + kept`, then `d`'s ticket drawn last. Then the
  full-ring pass, the entry at `head + used` under `d`'s ticket, and
  `rn_unreliable_sync`.
- **`Unlink(d)`**: `rn_endpoint_unreliable_unlink`: the ticket cleared,
  the descriptor freed, the sync. Allowed between a walk's steps, as the taken
  pass of `rn_endpoint_write_packet` does.
- **`DrawOther`**: a reliable enqueue's `rn_descriptor_enqueue_stamp`
  (`rn_channels_finish` and `rn_ordered_announce_config` in
  `sdk/rocketnet_channel.cpp`), a draw from the same counter.
- **`WalkBegin`**, **`WalkNext`**: `rn_unreliable_walk_begin` and
  `rn_unreliable_walk_next`: the head and count when the walk began, the
  next live entry from the step.
- **`WalkStop`**: the caller ending a walk early
  (`rn_endpoint_unreliable_items`: the record limit, or a large sequenced
  record).

## Invariants

- **`LiveIsOrder`**: the live entries read from the head are exactly the queue
  in push order. A descriptor with two live entries, or one moved, breaks it.
- **`HeadSynced`**: the head entry is live whenever the ring is not empty.
- **`WalkExact`**: a walk returns only descriptors queued when it began, each
  at most once and in queue order.

## The instance

`MCRNUnreliableRingLayout.cfg`: three descriptors in a ring of six (the code's
`2 * queue`), `TicketMod = 4`, so the shared counter wraps every three draws
and every renumber can itself wrap. The control uses `VIEW View`: a ring cell
outside `[head, head + count)` that no open walk covers is written before it is
read, and a closed walk's fields are all written by `WalkBegin`, so the view
leaves those values out. The witnesses run without the view; each also fails
under it (checked on hulk). Two descriptors in a ring of four pass without the
view (5,298,260 distinct states, 86 s); that instance does not reach the third
or fourth witness, which need a third descriptor.

## Reversed witnesses

The first three fail with `LiveIsOrder`; the fourth fails with
`WalkExact`. Every counterexample was checked against the C by hand.

1. **`MCRNUnreliableRingLayoutBrokenNoRenumber.cfg`** (`Renumber = FALSE`, a
   push after a ticket wrap that renumbers nothing). TLC: push 1 (ticket 1),
   push 2 (ticket 2), unlink 2 (a tombstone behind the live head, so the sync
   keeps it), push 3 (ticket 3, the counter wraps to 1), a reliable enqueue
   draws 1, push 2 again: its stamp is 2, the tombstone's ticket, so the ring
   reads 1, 2, 3, 2 with both entries of 2 live. Against the C:
   `rn_unreliable_walk_next` returns 2 twice and before 3, and
   `rn_endpoint_unreliable_items` packs its record twice, out of order. In the
   code the same trace needs `2^32 - 2` draws between the unlink and the
   second push with the head still queued.
2. **`MCRNUnreliableRingLayoutBrokenStaleSurvivors.cfg`** (`FreshTickets =
   FALSE`): the renumber drops tombstones but leaves the survivors' tickets.
   TLC: push 1 (1), push 2 (2), push 3 stamps 3 and wraps; the renumber keeps
   1 and 2 under tickets 1 and 2 from the last lap and gives 3 ticket 1;
   unlink 2 leaves a tombstone of ticket 2; push 2 is stamped 2, the next
   ticket of this lap, and the tombstone revives. Against the C: without a
   fresh ticket for each survivor in `rn_unreliable_renumber` the ring keeps
   last lap's tickets, which the counter reaches again before the next wrap
   sets the flag; that is why every survivor is renumbered.
3. **`MCRNUnreliableRingLayoutBrokenKeepPushed.cfg`** (`DropPushed = FALSE`):
   the renumber without `entry.descriptor == d ||`. TLC: after pushes of 1, 2,
   3 (the third wraps), an unlink of 2 and a reliable enqueue's draw of 1,
   push 2 is stamped 2 and renumbers: 1 gets 3 (the counter wraps again), 2's
   tombstone is live under its stamp, so it is kept and gets 1, 3 gets 2, and
   2's new entry gets 3. Unlink 2 leaves tombstones (2, 1) and (2, 3). Push 2
   is stamped 1, reviving (2, 1); kept, it is renumbered 3, which revives (2,
   3) further on; kept, it is renumbered 2; the final draw gives 2 ticket 3,
   which makes the entry at position 1 live again, and the new entry is
   appended under 3: the live entries read 1, 2, 3, 2 (positions 0, 1, 2 and
   4). Against the C: an entry naming `d` is stale by construction (`d` was
   free until this send), but its ticket can equal `d`'s new stamp or a ticket
   the renumber draws; dropping them all in `rn_unreliable_renumber` is what
   keeps them dead.

For any other descriptor the renumber needs no such test: its tombstones lie
before its one live entry, so the loop reaches them, still dead, before it
gives that entry a new ticket.

4. **`MCRNUnreliableRingLayoutBrokenWalkFromHead.cfg`** (`WalkSnapshot =
   FALSE`): the walk reads from the current head instead of the head
   captured at its start. TLC: push 1 (ticket 1), push 2 (ticket 2), push 3
   (ticket 3, the counter wraps), unlink 2, unlink 3, then push 1 again:
   the renumber writes the survivors (1 under ticket 2, 3 under ticket 3)
   at positions 0 and 1 and leaves 3's old copy (3, ticket 3) at position
   2, outside the live range (`count = 2`). A walk begins there (base 0,
   span 2, queue 1, 3); unlink 1 and its sync move the head from 0 to 1
   and drop `count` to 1. The broken walk re-reads from the moved head
   over its old span: position 1 returns 3, then position 2 returns 3
   again, still live under `ticket[3] = 3`, so `wseen = <<3, 3>>` and
   `WalkExact` fails. Against the C: `rn_unreliable_walk_next` reads
   `(walk->base + walk->step) % entries`, the head as it stood when
   `rn_unreliable_walk_begin` ran; reading
   `endpoint->unreliable_ring_head` instead reaches the stale copy the
   renumber left beyond `unreliable_ring_used` once `rn_unreliable_sync`
   has advanced the head past an unlinked entry, and the same record is
   packed twice.
