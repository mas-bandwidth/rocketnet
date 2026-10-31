# RNChannelQueuesLayout: the channel FIFOs and the ready list at the code's links

`RNChannelQueuesLayout.tla` models the send side's per-channel reliable FIFOs
and the ready list as `sdk/rocketnet_channel.cpp` lays them out. Each FIFO is
intrusive: `queue_head` and `queue_tail` in the channel's `rn_channel_send_t`,
`queue_next` in each send descriptor. The ready list is doubly linked by
channel index through `ready_prev` and `ready_next`, with `ready_head` and
`ready_tail` in `rn_channels_t`; a member's name is its `ready_ticket` (from
`ready_ticket_next`, which skips 0 on a wrap) and the head generation it
captured when it joined.

`RNChannelQueues.tla` checks the scheduling of these structures as sequences.
A sequence cannot hold a stale tail, a missing end pointer or a link written
into a descriptor that already left its FIFO; the links can, and that is what
this model carries. Its ghost state is that abstract one, a sequence per FIFO
and one for the ready list, and the invariants check that the links walk
exactly those sequences.

## The instance

`MCRNChannelQueuesLayout.tla`: three send descriptors, two channels, ready
tickets wrapping at 4 (skipping 0) and descriptor generations wrapping after
2 to 1.

## Actions and the code they mirror

Line numbers in `sdk/rocketnet_channel.cpp` at this commit.

| Action | Code |
|---|---|
| `Enqueue(c, d)` | `rn_channels_finish` (1247-1314): the descriptor taken (`rn_region_descriptor_take` clears it whole and bumps its generation, skipping 0; `rn_descriptor_enqueue_stamp` sets `queue_next` to NONE), `queued` counted, `rn_channel_queue_append` (1084-1093), and `rn_ready_push` at the back when the channel had nothing queued (1311) |
| `Config(d)` | `rn_ordered_announce_config` (1338-1403): `rn_channel_queue_push_front` on channel 0 (1095-1102), its ready membership removed by name, `rn_ready_push_front` (once: `config_announced`) |
| `WalkEmpty` | `rn_ordered_write` (1678-1687): a ready channel with nothing queued leaves the turns |
| `Number` | `rn_ordered_write` (1689-1729): the head channel leaves the list under the ticket and generation it joined with (`rn_ready_remove_named`, 1168-1191), the FIFO head is popped (`rn_channel_queue_pop`, 1104-1112) and the channel rejoins the back if it has more (`rn_ready_link`, 1126-1159, reading `rn_channel_head_generation`, 1114-1119) |
| `Free(d)` | environment: the allocator frees a numbered descriptor on its ack (`rn_region_send_free`, modelled by `RNSendStoreLayout`); it touches no queue link |

The window stall (`numbers == 0`) changes nothing and is the stutter. A
binding's reset (`rn_channels_bind`) empties every queue at once and is left
to `RNChannelQueues` (`Reset`). `WalkEmpty` is a defensive path: with the
code's own callers no ready channel is ever empty (`NonEmptyIsReady` and
TLC's reachable states agree), so it is kept for fidelity and is unreachable
in the instance.

## Invariants

| Invariant | Meaning |
|---|---|
| `TypeOK` | types |
| `QueueIsList` | following `queue_next` from each `queue_head` gives exactly that channel's FIFO; `queue_tail` is its last entry (NONE when empty); `queued` is its length |
| `NoForeignLink` | no `queue_next` is ever written into a descriptor that is not that FIFO's last entry, and a pop takes the FIFO's first |
| `ReadyIsList` | `ready_next` from `ready_head` and `ready_prev` from `ready_tail` walk the ready list forwards and backwards |
| `MembershipByTicket` | a member holds a nonzero ticket; a nonmember holds none and has no links |
| `NonEmptyIsReady` | a channel with work queued takes turns |
| `QueuedOwned` | a queued descriptor sits in exactly one FIFO |

## Reversed witnesses

| Case | Line removed | Declared invariant |
|---|---|---|
| `MCRNChannelQueuesLayoutBrokenPopKeepsTail.cfg` | `PopResetsTail = FALSE`: `queue_tail = NONE` after popping the last entry (1110-1111) | `NoForeignLink` |
| `MCRNChannelQueuesLayoutBrokenPushFrontNoTail.cfg` | `PushFrontSetsTail = FALSE`: `queue_tail = d` on an empty FIFO (1100-1101) | `QueueIsList` |
| `MCRNChannelQueuesLayoutBrokenRemoveKeepsEnds.cfg` | `RemoveUpdatesEnds = FALSE`: the `else` branches that move `ready_head` and `ready_tail` (1178-1183) | `ReadyIsList` |

### Hand checks against the code

**PopKeepsTail.** TLC's trace: descriptor 0 is queued on channel 0 (head and
tail 0, channel 0 ready), then numbered: the pop sets `queue_head` to NONE
but leaves `queue_tail` at 0, and descriptor 0 is now in the send window.
Descriptor 1 is queued on channel 0: `queued` was 0, so the channel rejoins
the ready list, and `rn_channel_queue_append` sees `queue_tail != NONE` at
line 1088 and writes `send_descriptors[0].queue_next = 1` into the numbered
descriptor (or, after its ack, a freed or reused one), while `queue_head`
stays NONE. The next walk finds channel 0 ready with an empty FIFO and drops
it (1683-1687): descriptor 1 is never numbered.

**PushFrontNoTail.** TLC's trace: CONFIG is announced on an empty channel 0.
`queue_head` becomes the CONFIG descriptor and `queue_tail` stays NONE. The
next append to channel 0 finds `queue_tail == NONE` and sets `queue_head` to
the new descriptor (line 1091), unlinking CONFIG, which is never numbered.
TLC reports the broken list at the push itself.

**RemoveKeepsEnds.** TLC's trace: channel 0 joins the ready list (head and
tail 0). CONFIG is announced: `rn_ordered_announce_config` removes channel 0
by name; with neither end moved, `ready_head` and `ready_tail` still name
channel 0. `rn_ready_push_front` then sets `ready_next[0] = ready_head = 0` and
`ready_prev[ready_head] = 0`: channel 0 links to itself, and the walk at
line 1678 would number channel 0 forever while no other channel ever gets a
turn.

## Records

See `RUNS.tsv` (group `rnchannelqueueslayout`).
