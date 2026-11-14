# RNAckApplyPhase: the deferred ack apply

`RNAckApplyPhase.tla` models the deferred apply of the server's received acks
in `sdk/rocketnet_ack.cpp` (`rocketnet_server_acks_t` with the deferral on),
the state behind the update in three calls: `rocketnet_server_update_receive`
on the main thread, `rocketnet_server_update_client` for each client on the
game threads, `rocketnet_server_update_finish` on the main thread. The model
was written from the code, and the trace check binds the code to it
(`TraceRNAckApplyPhase.tla`, `tests/tla_trace_ack_apply.cpp`, layer
`ackapply` of `tools/tla_trace_check.py`).

## The code and the model

| Code (`sdk/rocketnet_ack.cpp`) | Model |
| --- | --- |
| `rocketnet_server_acks_begin_receive`: the batch empties, the phase is RECEIVING | `BeginReceive` |
| `rocketnet_server_acks_receive_block`, deferred, RECEIVING: the block is written at `pending[num_pending]` with its ranges at `num_ranges` in the range store, and linked at its client's tail; when the queue is full or the range store cannot take its ranges, `rocketnet_server_acks_gather` runs first. A duplicate is queued like any other block | `Receive(c, b, r)` |
| the same call while CLOSED: `rocketnet_server_acks_apply_inline` applies the block at once and appends its ids to the batch | `Receive(c, b, r)`, the closed branch |
| the same call with the empty block (`largest` 0): queues nothing, and applied inline acks nothing | `Receive(c, 0, r)`: nothing changes |
| `rocketnet_server_acks_settle`: the main thread applies one client's chain where it stands (asserted RECEIVING); a client with no chain returns at once, in any phase | `Settle(c)` |
| `rocketnet_server_acks_open`: the claim of every client with a chain is set, the phase is OPEN | `Open` |
| `rocketnet_server_acks_apply_client`: one exchange of the claim; set, the caller applies the client's chain and returns the blocks applied | `Take(t, c, n)` |
| the same call finding the claim clear while OPEN (nothing queued, or taken already): returns 0 | `TakeRefused(t, c)` |
| the same call in any other phase, where no claim stands: returns 0 | `TakeOutside(t, c)` |
| `rocketnet_server_acks_close`: `rocketnet_server_acks_gather` applies every client that still has a chain, clears its claim, and copies each entry's ids from its client's segment into the batch in arrival order; the queue empties, the phase is CLOSED | `Close` |

The server transport calls these from `rocketnet_server_transport_update_receive`
(`BeginReceive`, the `Receive`s and `Settle`s of its receive loop, `Open`),
`rocketnet_server_transport_update_client_acks` (the take) and
`rocketnet_server_transport_update_finish` (`Close`). `rocketnet_server_update`
in one call is `Open` then `Close` with no take between. The transport also
refuses, and counts as misuse, a take outside `update_receive` ..
`update_finish` before it reaches the acks (its `update_open`); the model's
`TakeOutside` is what the acks do with such a call on their own.

The code applies inline in the OPEN phase too; the model assumes no block
arrives there, and that is the environment's: both callers of
`rocketnet_server_receive_one` are inside
`rocketnet_server_transport_update_receive`, before
`rocketnet_server_acks_open`.

The variables are the code's: `phase`; `queue`, the entries
`pending[0 .. num_pending)` with each entry's client, block, ranges, range
offset, `next`, the offset and count of its ids in its client's segment, and
`done`, whether it was applied (the code keeps no word for it: an applied
entry is one no longer on its client's chain); `numRanges`; per client
`head`, `tail`, `claim` and `segment` (the ids its applied blocks gave since
the last gather); `batch`, the published ids with their client indexes; per
client `acked`, the blocks its endpoint has acknowledged (the sent records
marked acked). Indexes are the code's: from 0, -1 for none. Three variables
are history the code does not keep: `took`, the threads that applied each
client since the last `Open` (the code keeps only the claim); `arrived`, the
blocks that arrived new (not acknowledged, not arrived already) since the
receive began, in arrival order (the queue forgets it at the gather); and
`gathered`, what forced the receive's latest early gather, the full queue or
only the range store (it names the trace check's reach predicates; nothing
else reads it).

A block is one id: applying an entry appends its block to its client's
segment, unless the endpoint has acknowledged that block already (before, or
earlier on the same chain): a duplicate block gives no id (`ids_count` 0),
because `rocketnet_ack_endpoint_process_block` visits only the sent records
not yet acknowledged. The rest of the endpoint's state is a function of the
blocks its client applied, in order, and `SegmentInOrder` says that order is
arrival order whoever applied them.

## Invariants

| Invariant | Meaning |
| --- | --- |
| `OneTaker` | a client is applied by one thread in one open phase |
| `ClaimOnlyOpen` | a claim stands only while open, and only on a client with a chain: a take outside the open phase finds nothing |
| `ClosedEmpty` | outside an update nothing is queued, claimed or held in a segment |
| `Whole` | the batch followed by the queued entries that give or will give an id is what arrived new, in arrival order: nothing lost, nothing twice, a duplicate gives nothing |
| `Published` | after the finish the batch is the arrival order of the new blocks, the batch applying each block where it arrives publishes: the result does not depend on who took what |
| `ChainWhole` | a client's chain is its unapplied entries in arrival order, and its tail is the last |
| `SegmentInOrder` | a client's segment is its applied entries' ids in arrival order, each at the offset its entry recorded |
| `InBounds` | the queue and the range store stay inside their arrays, each entry's ranges starting where the entries before it end |

## The instance

Two committed runs, each two clients, the main thread and two game threads,
a queue of three entries and a range store of two ranges, any block arriving
any number of times, and the empty block:

- `MCRNAckApplyPhase.cfg`: two blocks a client, each of one range, so the
  range store forces the early gather (the queue never fills). 1,693,400
  states generated, 131,302 distinct, 9 s.
- `MCRNAckApplyPhaseOneBlock.cfg`: one block a client (every later copy a
  duplicate), with no ranges or one, so the full queue forces it too.
  1,115,580 generated, 85,616 distinct, 6 s.

Two blocks a client with both range counts is past 3,000,000 distinct states
at three minutes, outside the case budget.

## Reversed witnesses

| Config | Rule turned off | Code line removed | Fails |
| --- | --- | --- | --- |
| `MCRNAckApplyPhaseBrokenDuplicateSurfaces.cfg` | a block the endpoint has acknowledged gives no id again | the walk over `sent_unacked` that feeds `visit` in `rocketnet_ack_endpoint_process_block`, every slot of the interval visited in its place | `Published` |
| `MCRNAckApplyPhaseBrokenLoadTake.cfg` | the take clears the claim in the step that reads it | the `claim.exchange( 0 )` of `rocketnet_server_acks_apply_client`, a load in its place | `OneTaker` |
| `MCRNAckApplyPhaseBrokenSettleWhileOpen.cfg` | settle runs only in the receive | the `rocketnet_assert( acks->phase == ROCKETNET_SERVER_ACKS_RECEIVING )` of `rocketnet_server_acks_settle` | `OneTaker` |
| `MCRNAckApplyPhaseBrokenCloseSkipsUntaken.cfg` | the gather applies every client that still has a chain | the `if ( client->head >= 0 )` block of the first loop of `rocketnet_server_acks_gather` | `Whole` |
| `MCRNAckApplyPhaseBrokenCloseKeepsClaim.cfg` | the gather clears the claim of each client it applies | `client->claim.store( 0 )` in that block | `ClaimOnlyOpen` |
| `MCRNAckApplyPhaseBrokenNoGatherWhenFull.cfg` (layout) | a block that does not fit gathers first | the `if ( acks->num_pending == acks->pending_capacity \|\| ... )` gather of `rocketnet_server_acks_receive_block` | `InBounds` |
| `MCRNAckApplyPhaseBrokenTailNotAdvanced.cfg` (layout) | a queued block becomes its client's tail | `client->tail = e;` of `rocketnet_server_acks_receive_block` | `ChainWhole` |

Each counterexample was checked against the C by hand:

- duplicate-surfaces: block 1 of client 0 arrives while CLOSED and is applied
  inline, `<<0, 1>>` into the batch; the same block arrives again, still
  CLOSED, and with the acknowledged record visited again its id goes into
  the batch a second time, so the batch is not the arrival order of the new
  blocks.
- load-take: a block for client 0, `Open`, then two threads call
  `apply_client( 0 )`; with a load each reads the claim 1 and each enters
  `rocketnet_server_acks_apply_queued` for client 0.
- settle-while-open: a block for client 0, `Open`, the main thread's
  `settle( 0 )` applies the chain with the claim still set, and a game
  thread's `apply_client( 0 )` then wins the exchange: two threads on one
  client's sent side in one open phase. The assert is the code's only word
  on this rule; settle's callers are in the receive loop, which the caller's
  contract keeps out of the open phase.
- close-skips-untaken: a block for client 0, then `close`; without the apply
  the entry's `ids_count` stays 0, the second loop copies nothing, and
  `num_pending = 0` drops the block.
- close-keeps-claim: a block for client 0, `Open`, `close`; the gather
  applies client 0 and its claim stays 1 into CLOSED, so the next update's
  `apply_client( 0 )` wins the exchange while the main thread is still in
  its receive loop.
- no-gather-when-full: three blocks of one range each; the third writes its
  range at `ranges + 2 * 2`, past a store of two.
- tail-not-advanced (fails at the second block, on the tail conjunct of
  `ChainWhole`): two blocks for client 0; the second is linked at
  `pending[0].next` and the tail stays 0, so a third would overwrite that
  link and the second would fall off the chain.

## Not modelled

The bare protocol (no deferral: every block inline); a block in the OPEN
phase, which the callers never send (above); the per-client counters the
gather adds; `rocketnet_server_acks_reset_slot`, which asserts its client
settled; the endpoint's own state (`SessionAcks.tla` and the ack layout
models). Two threads inside one call at once are TSan's
(`tests/functional/ack_apply_threads.cpp`): the model and the trace check
order whole calls.

## The trace check

`tests/tla_trace_ack_apply.cpp` walks the code with three clients, the main
thread and two real game threads, one call a step, and TLC checks each trace
against `TraceRNAckApplyPhase.tla`. Every field is read from the code
(`done` from each entry's place on or off its client's chain, `acked` from
the endpoints' sent records) but `took`, which the walk keeps from the
takes' return values, `arrived`, which it keeps from the blocks it hands in
and the endpoint's records before each, and `gathered`, which it reads from
the queue and range store before each block. Some of its blocks are
duplicates of the client's earlier blocks, some the empty block, some arrive
while CLOSED and are applied inline, and it settles clients with no chain in
every phase. The code's queue is 8192 entries
and 65536 ranges, which 400 steps do not fill, so the walk lowers it first
through the seam `rocketnet_server_acks_lower_capacity_for_test` to 6
entries and one block's 255 ranges, and some of its blocks carry 100 ranges:
the early gather then runs in the code, forced by the full queue in some
receives and by the range store in others. The row's reach predicates
`GatheredQueueFull` and `GatheredRangesFull` require each in an accepted
clean trace, beside `TakenByGameThread`, `LeftForFinish`, `Chained`,
`QueuedBehindSettled` and `AppliedDuplicate` (an applied entry that gave no
id).
