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
| `rocketnet_server_acks_receive_block`, deferred: the block is written at `pending[num_pending]` with its ranges at `num_ranges` in the range store, and linked at its client's tail; when the queue is full or the range store cannot take its ranges, `rocketnet_server_acks_gather` runs first | `Receive(c, b, r)` |
| `rocketnet_server_acks_settle`: the main thread applies one client's chain where it stands (asserted RECEIVING) | `Settle(c)` |
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

The variables are the code's: `phase`; `queue`, the entries
`pending[0 .. num_pending)` with each entry's client, block, ranges, range
offset, `next`, and the offset and count of its ids in its client's segment;
`numRanges`; per client `head`, `tail`, `claim` and `segment` (the ids its
applied blocks gave since the last gather); `batch`, the published ids with
their client indexes. Indexes are the code's: from 0, -1 for none. Two
variables are history the code does not keep: `took`, the threads that
applied each client since the last `Open` (the code keeps only the claim),
and the ghost `arrived`, the arrival order since the receive began (the
queue forgets it at the gather).

A block is one id: applying an entry appends its block to its client's
segment. The endpoint a block is applied to is not modelled; its state is a
function of the blocks its client applied, in order, and `SegmentInOrder`
says that order is arrival order whoever applied them.

## Invariants

| Invariant | Meaning |
| --- | --- |
| `OneTaker` | a client is applied by one thread in one open phase |
| `ClaimOnlyOpen` | a claim stands only while open, and only on a client with a chain: a take outside the open phase finds nothing |
| `ClosedEmpty` | outside an update nothing is queued, claimed or held in a segment |
| `Whole` | the batch followed by the queue is what arrived, in arrival order: nothing lost, nothing twice |
| `Published` | after the finish the batch is the arrival order, the batch applying each block where it arrives publishes: the result does not depend on who took what |
| `ChainWhole` | a client's chain is its unapplied entries in arrival order, and its tail is the last |
| `SegmentInOrder` | a client's segment is its applied entries' ids in arrival order, each at the offset its entry recorded |
| `InBounds` | the queue and the range store stay inside their arrays, each entry's ranges starting where the entries before it end |

## The instance

`MCRNAckApplyPhase.cfg`: two clients, the main thread and two game threads,
two blocks a client an update with up to one range each, a queue of three
entries and a range store of two ranges, so the early gather runs for both
reasons. 410,520 states generated, 41,985 distinct.

## Reversed witnesses

| Config | Rule turned off | Code line removed | Fails |
| --- | --- | --- | --- |
| `MCRNAckApplyPhaseBrokenLoadTake.cfg` | the take clears the claim in the step that reads it | the `claim.exchange( 0 )` of `rocketnet_server_acks_apply_client`, a load in its place | `OneTaker` |
| `MCRNAckApplyPhaseBrokenSettleWhileOpen.cfg` | settle runs only in the receive | the `rocketnet_assert( acks->phase == ROCKETNET_SERVER_ACKS_RECEIVING )` of `rocketnet_server_acks_settle` | `OneTaker` |
| `MCRNAckApplyPhaseBrokenCloseSkipsUntaken.cfg` | the gather applies every client that still has a chain | the `if ( client->head >= 0 )` block of the first loop of `rocketnet_server_acks_gather` | `Whole` |
| `MCRNAckApplyPhaseBrokenCloseKeepsClaim.cfg` | the gather clears the claim of each client it applies | `client->claim.store( 0 )` in that block | `ClaimOnlyOpen` |
| `MCRNAckApplyPhaseBrokenNoGatherWhenFull.cfg` (layout) | a block that does not fit gathers first | the `if ( acks->num_pending == acks->pending_capacity \|\| ... )` gather of `rocketnet_server_acks_receive_block` | `InBounds` |
| `MCRNAckApplyPhaseBrokenTailNotAdvanced.cfg` (layout) | a queued block becomes its client's tail | `client->tail = e;` of `rocketnet_server_acks_receive_block` | `ChainWhole` |

Each counterexample was checked against the C by hand:

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
- tail-not-advanced: two blocks for client 0; the second is linked at
  `pending[0].next` and the tail stays 0, so a third would overwrite that
  link and the second would fall off the chain.

## Not modelled

The inline apply (a bare protocol, or a block received outside the receive
phase); the empty block, which queues nothing; the per-client counters the
gather adds; `rocketnet_server_acks_reset_slot`, which asserts its client
settled; the endpoint's own state (`SessionAcks.tla` and the ack layout
models). Two threads inside one call at once are TSan's
(`tests/functional/ack_apply_threads.cpp`): the model and the trace check
order whole calls.

## The trace check

`tests/tla_trace_ack_apply.cpp` walks the code with three clients, the main
thread and two real game threads, one call a step, and TLC checks each trace
against `TraceRNAckApplyPhase.tla`. Every field is read from the code but
`took`, which the walk keeps from the takes' return values. The walk's
queue is the code's 8192 entries and 65536 ranges, which 400 steps do not
fill: the early gather is checked by the instance above and its witness, not
by the trace.
