# RNXdpReplay: kernel replay protection with a read-only high-water map

`tla/RNXdpReplay.tla` models kernel replay protection for the XDP server
transport: a BPF hash map `server_xdp_replay_map` that the XDP program
(`server_xdp/server_xdp.c`) only reads, and that the transport writes once per
server update from the session replay windows (`sdk/rocketnet_replay.h`,
`highest_opened`). The worked example of a replay model is
`tla/ReplayWindow.tla`; that module is the userspace window, this one is the
kernel's coarse filter in front of it.

The code cites this module at each step. `KernelFilter` is the replay rule
of `server_xdp_udp` in `server_xdp/server_xdp.c`, which only looks
`server_xdp_replay_map` up. `Walk` is `rocketnet_server_replay_marks_walk`
(`sdk/rocketnet_server_replay_marks.cpp`), called from
`rocketnet_server_transport_update` in `sdk/rocketnet_server_transport.cpp`
after the maintenance sweep, and `Commit` is the transport's `replay_commit`,
on Linux `rocketnet_server_socket_replay_commit`
(`sdk/rocketnet_server_socket_linux.cpp`), which applies every forget and
then every mark to the pinned map.

`tla/MCRNXdpReplay.tla` and `tla/MCRNXdpReplay.cfg` are the small instance.
`tla/MCRNXdpReplayBrokenKernelAdvances.cfg` and
`tla/MCRNXdpReplayBrokenNoForget.cfg` are the reversed witnesses. The cases
are rows of `tla/CASES.tsv` (group `rnxdpreplay`) with measured records in
`tla/RUNS.tsv`.

## The rule

- The map: `ROCKETNET_XDP_REPLAY_MAX_ENTRIES` (1024) entries, keyed by the
  source family, port and address, valued by a 32-bit `high_water`.
- The kernel rule, for the session packet types only (keep-alive, payload,
  disconnect, ack, fragment), after the type and size check and before the
  redirect: look the source up; with an entry, drop when
  `(int32_t)(high_water - sequence) >= ROCKETNET_XDP_REPLAY_WINDOW` (16),
  counting `ROCKETNET_SERVER_XDP_COUNTER_DROPPED_REPLAYED`; otherwise go on.
  The kernel never updates or deletes an entry, so a forged sequence far
  ahead passes to userspace, fails the AEAD open there, and moves nothing.
- Userspace, once per update after packet processing and before the driver
  flush: FORGET the old address of every slot that held a mark and is no
  longer connected (or whose mapping changed address); then MARK every
  connected slot whose `highest_opened` is nonzero and differs from what it
  last marked. The driver applies all forgets, then all marks, in one commit.
  The map lags the truth by at most one update and is never ahead of it.
- Sizing: the kernel window is 16 behind the high-water; the userspace window
  stays `ROCKETNET_REPLAY_WINDOW_BITS` (4096). A genuine packet within 16 of
  the last opened sequence is never dropped in the kernel.

## What the module covers

Constants: `Addresses` (client source address and port), `MaxSeq` (the
highest sequence sent or forged), `W` (the kernel window), `Sessions` (the
sessions over the whole run, so a reconnect from one address is reachable),
`MaxInFlight` (the bound on packets in the network).

| Variable | Code | Meaning |
|---|---|---|
| `session[a]` | the slot's connection | the live session id, `0` for none |
| `sent[a]` | client send sequence | highest genuine sequence the live session sent |
| `opened[a]` | `highest_opened` of `replay_session` | raised only after a packet opens |
| `marked[a]` | the transport's per-slot record | what it last marked, `0` for none |
| `kmap[a]` | `server_xdp_replay_map` entry | `hw` is `high_water`, `0` for no entry; `sess` is a ghost naming the session the mark was taken from |
| `net` | packets in flight | each `[addr, seq, session, genuine, stage, copy]`; `stage` is `"wire"` before the kernel, `"user"` after it passed; `copy` lets a duplicate coexist with its original |
| `pendingForget`, `pendingMark` | the walk's output | what the next commit applies |
| `phase` | the update | `"run"` while packets are processed, `"flush"` between the walk and the commit |
| `settle[a]` | (assumption bookkeeping) | `"released"`, then `"walked"`, then `"clear"` after a Release |
| `reached[s]` | (ghost) | the highest `highest_opened` session `s` reached |
| `dropped` | (ghost) | genuine packets the kernel dropped, with the session of the mark it dropped them against |
| `nextSession` | (ghost) | the id the next connection takes |

Two rule switches are definitions with the code's values, each overridden
by one witness configuration: `KernelAdvances` (`FALSE`) and `WalkForgets`
(`TRUE`).

Actions, each against the code it models:

- `Connect(a)`: a new session; sequences restart at 1 and `highest_opened` is
  0. Enabled only when `settle[a] = "clear"`, that is once a `Walk` and a
  `Commit` have both run since the last `Release(a)` (the assumption below).
- `ClientSend(a)`: the live session sends its next genuine sequence.
- `Forge(a, s)`: an attacker spoofing `a` sends any sequence `s`; it never
  opens.
- `Duplicate(p)`: the network delivers a second copy of a packet.
- `KernelFilter(p)`: THE KERNEL RULE. With an entry and `hw - seq >= W`, the
  packet is dropped (a genuine one is recorded in `dropped`); otherwise it
  passes to userspace. The code leaves `kmap` unchanged; the broken shape
  writes `kmap[a] := seq` on every pass. The kernel runs beside the update,
  so this is enabled in both phases.
- `UserOpen(p)`: a genuine packet of the live session opens and raises
  `highest_opened`; a forged one fails the AEAD open, and a packet of an
  ended session has no mapping: neither moves anything.
- `Release(a)`: the session ends.
- `Walk`: forgets first (every address with `marked[a] # 0` and no live
  session), then marks (every live session with `opened[a] # 0` and
  `opened[a] # marked[a]`), into the pending sets; `marked` records them.
- `Commit`: the driver applies all forgets, then all marks, to `kmap`. The
  only action that writes `kmap`.

`Connect`, `UserOpen`, `Release` and `Walk` are userspace steps of one
update, so they run only in phase `"run"`; `Commit` ends the flush.

Properties:

- `TypeOK`.
- `KernelNeverWrites`, an action property: every step but `Commit` leaves
  `kmap` unchanged.
- `MarkNeverAhead`: with a live session, the entry for its address is none,
  or was taken from that session and is at most its `highest_opened`. With
  no live session the forget may still be pending (the one-update lag).
- `MarksTakenFromTheTruth`: every entry is at most the `highest_opened` its
  session reached.
- `FreshGenuineNeverDropped`: a genuine packet of the live session dropped
  against a mark of its own session is never fresh: its sequence is at most
  `opened[a] - W`.
- `NewSessionNeverDroppedByOldMark`: no genuine packet is dropped against a
  mark taken in an earlier session.

The last two split one statement by the session of the mark: together they
say no genuine packet of the live session within `W` of its last opened
sequence is dropped in the kernel, and each reversed witness reaches exactly
one of them.

## Assumption: HandshakeSpansAnUpdate

A slot exists only after a connection request, a challenge and a response
round trip, while the walk and the commit run every server update. So
between the release of a session from an address and the next connection
from that address, at least one walk and one commit run. `Connect(a)` is
enabled only when that has happened. Without it, a reconnect inside one
update would find the old mark still committed for one update; the
assumption is what makes the one-update lag safe for a new session.

## Left out

- **The 32-bit wrap.** Sequences are naturals and `hw - seq` is integer
  subtraction; the code's `(int32_t)(high_water - sequence)` matches it while
  the two are within 2^31.
- **The byte layout.** The key (family, pad, port, address) and value
  (`high_water`, pad) structs in `sdk/rocketnet_wire.h`, the little-endian
  sequence after the 6-byte prefilter header, and the packet type list are
  not modelled; an address in `Addresses` stands for a key.
- **Map capacity.** `ROCKETNET_XDP_REPLAY_MAX_ENTRIES` is not modelled: a
  full map refuses a mark, which leaves no entry, and no entry passes.
- **Slots and addresses.** A slot is identified with its address. A slot
  whose mapping changes address forgets the old address exactly as a release
  does; a reconnect landing in a different slot is a reconnect here.
- **Addresses are independent.** Every action touches one address, so the
  small instance has one.
- **The userspace window.** `ROCKETNET_REPLAY_WINDOW_BITS` is wider than
  every sequence explored, so a genuine packet of the live session that
  reaches userspace always opens (a duplicate opens to the same maximum).

## Running

From the Rocketnet checkout, with the pinned `tlacheck` and JAR of
`tla/README.md`:

```sh
for i in 0 1 2; do
  "$job/tlacheck" run --root . --jar "$jar" --java "$java" \
    --group rnxdpreplay --shards 3 --shard "$i" --workers 2 --timeout 110s \
    --dir "$job/rnxdpreplay-$i"
done
"$job/tlacheck" merge --root . --keep tla/RUNS.tsv \
  --out "$job/RUNS.tsv" "$job"/rnxdpreplay-*/RUNS.tsv
"$job/tlacheck" groups --root . --stale
```

## The small instance

`MCRNXdpReplay.cfg`: one address, `MaxSeq = 5`, `W = 2`, `Sessions = 2`,
`MaxInFlight = 2`. TLC explores every send, forgery, duplicate, kernel pass
or drop, open, release, walk and commit interleaving: 2,331,306 distinct
states (6,939,416 generated, depth 52) in about 28 seconds on two workers,
and every invariant and `KernelNeverWrites` holds.

## Reversed witnesses

| Configuration | Rule removed | Expected failure |
|---|---|---|
| `MCRNXdpReplayBrokenKernelAdvances.cfg` | the kernel never writes: `KernelAdvances <- KernelAdvancesBroken`, `kmap[a] := seq` on every pass | `FreshGenuineNeverDropped`, exit 12, depth 6 |
| `MCRNXdpReplayBrokenNoForget.cfg` | the walk forgets a released slot: `WalkForgets <- WalkForgetsBroken` | `NewSessionNeverDroppedByOldMark`, exit 12, depth 17 |

Each witness configuration leaves out the properties that restate the rule it
removes, so the run reaches the harm rather than the rule itself. Run with
every property of the control, the same overrides fail one step after the
broken rule acts: the kernel-advances shape fails `MarksTakenFromTheTruth` at
depth 3 (a forged packet passes and its sequence becomes the entry), and the
no-forget shape fails `MarkNeverAhead` at depth 11 (the reconnect, with the
old session's entry still in the map). Both were bench experiments, not
cases.

### KernelAdvances, checked by hand against the code

1. `Connect(A1)`: session 1; `ClientSend(A1)` puts genuine sequence 1 on the
   wire.
2. `Forge(A1, 3)`: a forged sequence 3 on the wire.
3. `KernelFilter` on the forgery: no entry, so it passes, and the broken
   kernel writes `high_water = 3`.
4. `KernelFilter` on genuine 1: `3 - 1 >= 2`, dropped, with `highest_opened`
   still 0.

In the code the kernel never writes at step 3; the forgery reaches
userspace, fails the open, and the entry stays absent, so genuine 1 passes.
At the real window of 16 the forgery need only be 16 ahead of the genuine
traffic: one spoofed packet would silence a client.

### NoForget, checked by hand against the code

1. `Connect(A1)`: session 1 sends 1, 2, 3; the kernel passes 1 and 3, and
   userspace opens both (`highest_opened = 3`); 2 is still on the wire.
2. `Walk` marks 3; `Commit` writes `high_water = 3`.
3. `Release(A1)`; `Walk` (the broken walk forgets nothing); `Commit`.
4. `Connect(A1)`: session 2, whose sequences restart at 1; it sends 1.
5. `KernelFilter` on session 2's sequence 1: `3 - 1 >= 2`, dropped against
   session 1's mark.

In the code the walk at step 3 forgets `A1` and the commit deletes the
entry before session 2 can exist (HandshakeSpansAnUpdate), so session 2's
first packets pass. At the real window, a client reconnecting from the same
address and port would lose every packet until its sequence passed the old
session's high-water minus 16.
