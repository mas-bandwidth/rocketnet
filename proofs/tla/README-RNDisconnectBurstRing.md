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
| `cursor` | `disconnect_burst_cursor`, the offset from the head of the burst the pass seals next |
| `C` | `ROCKETNET_MAX_CLIENTS`, the ring's slots |
| `K` | `ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS` |

The ghosts name each burst by the order it was deferred (`bid[s]`), count the
packets sealed for it (`seals`), collect the bursts that left the ring
(`gone`) and the bursts sealed in the current pass (`pass`).

## Actions and the code they mirror

- **`Defer`**: `rocketnet_server_transport_disconnect_client_impl` when a
  burst already waits or the update's allowance is short of `K`: with
  `disconnect_burst_count < ROCKETNET_MAX_CLIENTS`,
  `rocketnet_server_transport_defer_disconnect_burst` writes slot
  `(head + count) % ROCKETNET_MAX_CLIENTS` with `sealed = 0` and counts it;
  with the ring full the new burst is dropped and the slot closed locally.
- **`SealStep`**: one iteration of
  `rocketnet_server_transport_drain_disconnect_bursts`: the cursor restarts
  at 0 once it reaches `count`, the burst at `(head + cursor) % MAX` seals its
  next packet, the cursor moves on, and each completed burst at the head leaves
  (the head moves modulo `MAX`, `count` falls, a nonzero cursor drops by one).

A drain ends when its allowance is spent or a send frame is refused. Either
leaves the ring and the cursor as they are, and the next update's drain
continues the pass, so any interleaving of `Defer` and `SealStep` is a run of
updates. The stop path's drain is the same loop with a larger allowance. The
immediate burst (no burst waiting, the allowance covering `K`) never touches
the ring.

## Invariants

- **`NoOverwrite`**: a deferred burst never overwrites a waiting one.
- **`SealOnlyLive`**: a seal is made only for a waiting burst, never for the
  slot past the tail.
- **`PassFair`**: no burst is sealed twice in a pass, and a pass ends only
  when every waiting burst was sealed in it.
- **`SealedFallFromHead`**: sealed counts never rise from the head to the
  tail, so the head's burst completes first and the pop at the head frees
  every completed burst.
- **`CursorInRange`**: the cursor is at most `count`.
- **`ExactSeals`**: each waiting burst's record holds its own seals, fewer
  than `K`, and each burst that left had exactly `K`.

A burst deferred during a pass joins that pass at the tail, so the bursts ahead
of it in the pass may have a redundant packet sealed before its first. The
bursts waiting when a pass starts (a disconnect of every client in one update)
all have their first packet sealed before any has its second. The comment on
the ring in `rocketnet_server_transport_t` states the stronger form for every
burst; `PassFair` is the rule the code keeps.

## The instance

`MCRNDisconnectBurstRing.cfg`: four slots, bursts of two packets, eight
disconnects, so the ring fills, drops a burst, wraps and refills.
`MCRNDisconnectBurstRingK3.cfg`: three slots, bursts of three, seven
disconnects, so waiting bursts are two seals apart. Both run without deadlock
checking (`ignore-terminal`): once every disconnect is made and every burst
has left, nothing is enabled. `SealStep` is enabled whenever a burst waits, so
no other state is terminal.

## Reversed witnesses

Each turns one rule off and must fail with the invariant it names. Every
counterexample below was checked against the C by hand.

1. **`MCRNDisconnectBurstRingBrokenFullOffByOne.cfg`**
   (`FullTestExact = FALSE`): the room test in
   `rocketnet_server_transport_disconnect_client_impl` as
   `disconnect_burst_count <= ROCKETNET_MAX_CLIENTS`. Violates `NoOverwrite`:
   three bursts fill the ring of three at head 0; a fourth writes slot
   `(0 + 3) % 3 = 0`, the waiting head's record and key.
2. **`MCRNDisconnectBurstRingBrokenCursorWrapLate.cfg`**
   (`CursorWrapAt = FALSE`): the drain restarting the cursor only at
   `cursor > count`. Violates `SealOnlyLive`: one burst waits; its first seal
   moves the cursor to 1, equal to `count`; the next iteration keeps it and
   seals slot `head + 1`, which holds no waiting burst.
3. **`MCRNDisconnectBurstRingBrokenCursorKept.cfg`** (`CursorDrops = FALSE`):
   the pop loop without `if ( disconnect_burst_cursor > 0 ) cursor--`.
   Violates `SealedFallFromHead`: burst 1 completes and leaves with the cursor
   at 1 and the ring empty; bursts 2 and 3 are deferred at offsets 0 and 1;
   the next seal goes to offset 1, so burst 3 has a packet sealed and burst 2,
   at the head, none.

## Runs

The records are in `RUNS.tsv` (group `rndisconnectburstring`), made with the
pinned runner as `proofs/tla/README.md` describes.
