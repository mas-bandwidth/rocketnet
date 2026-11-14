# RNDisconnectBurstRing

The server transport's ring of deferred disconnect bursts, at the layout of
`sdk/rocketnet_server_transport.cpp` and with the code's own arithmetic.
[ServerSession](README-ServerSession.md) counts the seals each ended session
still owes; this model carries the ring those bursts wait in: the head, the
count, the pass cursor, the modulo and the sealed counts.

## State

| Model | Code |
|---|---|
| `sealed[s]` | `disconnect_bursts[s].sealed` |
| `head`, `count` | `disconnect_burst_head`, `disconnect_burst_count` |
| `cursor` | `disconnect_burst_cursor`, the offset from the head of the burst the pass visits next |
| `SteadyCap` | `disconnect_burst_steady_cap`, which is `max_clients`: the most bursts a running server keeps waiting, and the most clients connected |
| `C` | `rocketnet_server_disconnect_bursts(max_clients)`, the ring's slots, which is `2 * max_clients` (`2 * SteadyCap`) and not `ROCKETNET_MAX_CLIENTS` |
| `conn` | the connected slots (`rocketnet_client_slots_connected`) |
| `stopping` | `rocketnet_server_stop` has run and the transport is `STOPPING` |
| `K` | `ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS` |
| `abandoned` | bursts marked complete because the destination was refused |
| `dropped` | ghost: bursts dropped before complete by a stop step with no send room |

The ghosts name each burst by the order it was deferred (`bid[s]`), count the
packets sealed for it (`seals`), collect the bursts that left the ring
(`gone`) and the bursts sealed in the current pass (`pass`).

## Actions and the code they mirror

- **`Connect`**: a client connects into a free slot while the server runs
  (outside this code: an environment action); at most `SteadyCap` are
  connected.
- **`Defer`**: `rocketnet_server_transport_disconnect_client_impl` on a
  running server (`shutdown` false) when a burst already waits or the
  update's allowance is short of `K`: while
  `disconnect_burst_count < disconnect_burst_steady_cap`,
  `rocketnet_server_transport_defer_disconnect_burst` writes slot
  `(head + count) % C` with `sealed = 0` and counts it; otherwise the new
  burst is dropped and the slot closed locally. There is no test against
  `C`. The immediate burst (no burst waiting, allowance and room for `K`)
  never touches the ring; its ring is the one a client that never connected
  leaves, so letting every disconnect defer only adds behaviours.
- **`Stop`**: `rocketnet_server_stop`: every connected slot is disconnected
  with `shutdown` true (`rocketnet_server_transport_disconnect_client_shutdown`),
  which defers its burst with no room test at all, one append after
  another at `(head + count) % C`; then the transport is `STOPPING`. It runs
  once (it returns at once unless the server is running), and after it no
  client connects and no running disconnect is made. The bursts waiting
  before it are at most `SteadyCap` (`SteadyWithinCap`) and it appends at
  most `SteadyCap`, so the ring of `2 * SteadyCap` holds them all: this is
  the whole of the code's no-overwrite argument. The only test of the ring's
  capacity in the code is
  `rocketnet_assert( disconnect_burst_count < disconnect_burst_capacity )`
  in `rocketnet_server_transport_defer_disconnect_burst`, compiled out of a
  release build; the model has no counterpart for it, and `NoOverwrite` is
  what it would catch.
- **`SealStep`**: one seal in
  `rocketnet_server_transport_drain_disconnect_bursts`: the cursor restarts
  at 0 once it reaches `count`, the incomplete burst at `(head + cursor) % C`
  seals its next packet, the cursor moves on, and each completed burst at the
  head leaves (the head moves modulo `C`, `count` falls, a nonzero cursor
  drops by one).
- **`Abandon`**: the same iteration when the start is refused for its
  destination with room left. The burst is marked complete (`sealed = K`) and
  no packet of it is sealed, then the head pop runs.
- **`PassOver`**: the same iteration when the entry is already complete. The
  cursor moves on and the entry is not sealed. An abandoned burst away from
  the head leaves only once the pop reaches it.
- **`DropForRoom`**: `rocketnet_server_transport_stop_step` (which returns
  unless the transport is `STOPPING`, so only after `Stop`) with one send
  queue (`Queues = 1`), when the
  transport reports no send room and is idle, and still no room when read
  again (`ServerStopSpread`'s `AbandonNoRoom`): every waiting burst is popped
  from the head with its key wiped, so `head` moves on by `count` modulo `C`
  and `count` is 0, then `disconnect_burst_cursor` is reset to 0. The sealed
  counts stay in the slots, as the code leaves them; the next `Defer` into a
  slot writes `sealed = 0`. Each burst not yet complete is dropped; one
  already complete (abandoned behind the head) leaves as it would have.

With several send queues (`Queues > 1`, `send_queues > 1` in the code) two
more steps are the code's:

- **`PassFull`**: the drain's iteration when the incomplete burst at the
  cursor has a full queue while another queue has room
  (`rocketnet_server_transport_queue_room` of its queue is 0, the total room
  is not): the cursor moves on, nothing is sealed and the pop does not run.
  The room itself is `ServerStopSpread`'s; here either outcome may happen.
  It can be the first iteration of a pass (the head's queue full), so it
  starts the new pass as a seal does.
- **`AbandonQueue`**: `rocketnet_server_transport_stop_step`'s several-queue
  branch, the transport idle: each incomplete waiting burst whose own queue
  is full (any nonempty set of them here) is marked complete where it sits
  and counted, then the complete bursts at the head leave, the cursor
  dropping with each. `DropForRoom` is not enabled with several queues, as
  the code takes this branch instead.

A drain ends when its allowance is spent or a send frame is refused for a
reason other than the destination. Either leaves the ring and the cursor as
they are, and the next update's drain continues the pass, so any interleaving
of `Connect`, `Defer`, `SealStep`, `Abandon`, `PassOver` and `PassFull`
before `Stop`, and of those seal steps with `DropForRoom` or `AbandonQueue`
after it, is a run of updates and then of stop steps. The stop path's drain
is the same loop with a larger allowance.

## Invariants

- **`NoOverwrite`**: a deferred burst never overwrites a waiting one.
- **`SteadyWithinCap`**: a running server keeps at most `SteadyCap` bursts
  waiting.
- **`SealOnlyLive`**: a seal is made only for a waiting burst, never for the
  slot past the tail.
- **`PassFair`** (one queue): no burst is sealed twice in a pass, and a pass
  ends only when every burst still waiting was sealed in it. An abandoned
  burst is complete, so the pass does not owe it a seal, and passing over it
  is not a second seal.
- **`PassFairUnlessFull`** (any number of queues): no burst is sealed twice
  in a pass, and a pass ends owing a seal only when it passed a burst over
  for its full queue.
- **`HeadNotComplete`** (any number of queues): the head is never left
  complete; every step that completes a burst pops the complete head.
- **`SealedFallFromHead`** (one queue): among bursts still waiting, sealed
  counts never rise from the head to the tail. A live entry already at `K`
  was abandoned and sits behind the head; the head itself is never left
  complete (`HeadNotComplete`).
- **`CursorInRange`**: the cursor is at most `count`.
- **`ExactSeals`**: a waiting burst that was not abandoned holds its own
  seals, fewer than `K` with one send queue. With several, a burst sealed
  whole can wait behind a head passed over for its full queue, since only
  the head pops, and then holds exactly `K` (its key stays until it reaches
  the head; the drain passes it over). An abandoned one is marked complete and was sent
  fewer than `K`. A burst that left ended exactly one way: sealed whole
  (exactly `K` sent), abandoned for its destination, or dropped for room
  (each fewer than `K` sent).

A burst deferred during a pass joins that pass at the tail, so the bursts ahead
of it in the pass may have a redundant packet sealed before its first. The
bursts waiting when a pass starts (a disconnect of every client in one update)
all have their first packet sealed before any has its second, with one send
queue. `PassFair` is the rule the code keeps with one queue. With several, a
burst whose queue is full is passed over while the bursts behind it are
sealed, so it can end a pass unsealed and fall behind them: `PassFair` and
`SealedFallFromHead` do not hold there (the two scope witnesses below), and
`PassFairUnlessFull` and `HeadNotComplete` are what the code keeps. The
comment on the ring in `rocketnet_server_transport_t` says the same.

## The instance

`MCRNDisconnectBurstRing.cfg`: a steady cap of two (four slots), bursts of
two packets, eight disconnects, so a running server reaches its cap and
drops a burst, the ring wraps, and the stop fills it to four.
`MCRNDisconnectBurstRingK3.cfg`: a steady cap of two, bursts of three, seven
disconnects, so waiting bursts are two seals apart. Both have one send queue.
`MCRNDisconnectBurstRingQueues.cfg`: a steady cap of two, bursts of two, six
disconnects and two send queues, with the invariants the code keeps there.
All three run without deadlock
checking (`ignore-terminal`): once every disconnect is made and every burst
has left, nothing is enabled. `SealStep` is enabled whenever a burst waits, so
no other state is terminal.

## Reversed witnesses

Each turns one rule off and must fail with the invariant it names. Every
counterexample below was checked against the C by hand.

1. **`MCRNDisconnectBurstRingBrokenSteadyCapOffByOne.cfg`**
   (`SteadyCapExact = FALSE`): the steady cap test in
   `rocketnet_server_transport_disconnect_client_impl` as
   `disconnect_burst_count <= disconnect_burst_steady_cap`. Violates
   `NoOverwrite` (TLC trace, 10 states, checked by hand against the C):
   two clients connect; three running disconnects (a client reconnecting
   between them) defer bursts 1, 2, 3 at slots 0, 1, 2, the third passing
   `2 <= 2`, so `count` is 3 with `max_clients` 2; two clients connect;
   `rocketnet_server_stop` calls the shutdown disconnect for each, and
   `rocketnet_server_transport_defer_disconnect_burst` writes slot
   `(0 + 3) % 4 = 3`, then slot `(0 + 4) % 4 = 0`: the waiting head's
   record and key, burst 1, overwritten by burst 5. The assert at the top
   of the defer is the only thing that would see it, in a debug build.
   (`ServerStopSpread`'s `steadycapoffbyone` is the same rule at the
   protocol's level; this one is at the ring's layout.)
2. **`MCRNDisconnectBurstRingBrokenCursorWrapLate.cfg`**
   (`CursorWrapAt = FALSE`): the drain restarting the cursor only at
   `cursor > count`. Violates `SealOnlyLive` (checked by hand): one burst waits; its first seal
   moves the cursor to 1, equal to `count`; the next iteration keeps it and
   seals slot `head + 1`, which holds no waiting burst.
3. **`MCRNDisconnectBurstRingBrokenCursorKept.cfg`** (`CursorDrops = FALSE`):
   the pop loop without `if ( disconnect_burst_cursor > 0 ) cursor--`.
   Violates `SealedFallFromHead` (8 states, checked by hand): burst 1 is
   deferred at slot 0, the stop appends bursts 2 and 3 at slots 1 and 2;
   burst 1's destination is refused, it is marked complete with the cursor
   moved to 1, and the pop takes it (head 1, count 2) with the cursor kept
   at 1; the next iteration seals slot `(1 + 1) % 4 = 2`, so burst 3 has a
   packet sealed and burst 2, at the head, none.
4. **`MCRNDisconnectBurstRingBrokenDropKeepsCursor.cfg`**
   (`DropResetsCursor = FALSE`): the drop for room without
   `disconnect_burst_cursor = 0`. Violates `CursorInRange` (checked by
   hand): one burst is deferred at slot 0, the server stops, its first seal
   moves the cursor to 1, and the drop
   pops it (head 1, count 0) with the cursor left at 1, past the empty ring.
   In the code the next drain's wrap test would hide it, but the stop step
   ends the stop there, and the drop is the one pop that does not lower the
   cursor with each burst it removes.
5. **`MCRNDisconnectBurstRingBrokenQueueAbandonNoPop.cfg`** (two queues,
   `AbandonQueuePops = FALSE`): the several-queue stop step without its pop
   loop. Violates `HeadNotComplete` (4 states, checked by hand): one client
   connects, the stop appends its burst at slot 0, and the stop step marks
   it complete for its full queue and leaves it at the head. The code's
   several-queue branch of `rocketnet_server_transport_stop_step` runs the
   head pop right after the marking loop.

Two scope witnesses show the one-queue rules do not hold with several
queues; each must fail, and neither is a code change:

- **`MCRNDisconnectBurstRingQueuesPassFair.cfg`** (`PassFair`, 5 states,
  checked by hand): one burst is deferred, its queue is full so the drain
  steps past it (cursor 1, count 1), and the next drain wraps the cursor to
  0 and seals it: the pass ended with that burst owed a seal. The drain's
  full-queue step in `rocketnet_server_transport_drain_disconnect_bursts`
  is this.
- **`MCRNDisconnectBurstRingQueuesSealedFall.cfg`** (`SealedFallFromHead`,
  6 states, checked by hand): the stop appends bursts 1 and 2, burst 1's
  queue is full and is stepped past, burst 2 is sealed, so the burst behind
  the head holds more seals than the head.

Not advancing the head on a drop is not a witness here: with `count` 0 every
head is a valid empty ring (`Defer` writes slot `head`, the pass starts
there), so in this layout it changes no outcome; the code advances it as the
pops do.

## Runs

Bench runs of this version (two workers, `distinct` states and seconds):

| Case | Result | Distinct | Seconds |
|---|---|---|---|
| `MCRNDisconnectBurstRing.cfg` | pass | 435,502 | 6.2 |
| `MCRNDisconnectBurstRingK3.cfg` | pass | 670,782 | 9.2 |
| `MCRNDisconnectBurstRingBrokenSteadyCapOffByOne.cfg` | `NoOverwrite`, exit 12 | 158 | 0.8 |
| `MCRNDisconnectBurstRingBrokenCursorWrapLate.cfg` | `SealOnlyLive`, exit 12 | 21 | 0.7 |
| `MCRNDisconnectBurstRingBrokenCursorKept.cfg` | `SealedFallFromHead`, exit 12 | 81 | 0.8 |
| `MCRNDisconnectBurstRingBrokenDropKeepsCursor.cfg` | `CursorInRange`, exit 12 | 25 | 0.7 |
| `MCRNDisconnectBurstRingQueues.cfg` | pass | 386,173 | 11.3 |
| `MCRNDisconnectBurstRingQueuesPassFair.cfg` | `PassFair`, exit 12 | 28 | 0.7 |
| `MCRNDisconnectBurstRingQueuesSealedFall.cfg` | `SealedFallFromHead`, exit 12 | 61 | 0.8 |
| `MCRNDisconnectBurstRingBrokenQueueAbandonNoPop.cfg` | `HeadNotComplete`, exit 12 | 15 | 0.7 |

The records are in `RUNS.tsv` (group `rndisconnectburstring`), made with the
pinned runner as `proofs/tla/README.md` describes.
