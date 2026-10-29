# RNXdpReplay: kernel replay protection with a read-only high-water map

`proofs/tla/RNXdpReplay.tla` is the design of kernel replay protection for the XDP
server transport: a BPF hash map `server_xdp_replay_map` that the XDP program
(`sdk/xdp/server_xdp.c`) only reads, and that the transport writes once per
server update from the session replay windows (`sdk/rocketnet_replay.h`,
`highest_opened`). The worked example of a replay model is
`proofs/tla/ReplayWindow.tla`; that module is the userspace window, this one is the
kernel's coarse filter in front of it.

The model was written before the code. `sdk/xdp/server_xdp.c` now holds
the replay map, names its steps after the actions below and cites this
module; the `xdp-replay-*` rows of `proofs/tla/COVERAGE.tsv` are `current`.

`proofs/tla/MCRNXdpReplay.tla` and `proofs/tla/MCRNXdpReplay.cfg` are the small instance;
`proofs/tla/MCRNXdpReplayReconnect.cfg` is the reconnect control and
`proofs/tla/MCRNXdpReplayRefusedMark.cfg` the refused-mark control.
`proofs/tla/MCRNXdpReplayBrokenKernelAdvances.cfg`,
`proofs/tla/MCRNXdpReplayBrokenNoForget.cfg`,
`proofs/tla/MCRNXdpReplayBrokenNarrowWindow.cfg`,
`proofs/tla/MCRNXdpReplayBrokenUnmarkedNotForgotten.cfg`,
`proofs/tla/MCRNXdpReplayBrokenFailedForgetDropped.cfg`,
`proofs/tla/MCRNXdpReplayBrokenSupersededForget.cfg` and
`proofs/tla/MCRNXdpReplayBrokenReconnectFreshness.cfg` are the reversed witnesses. The cases
are rows of `proofs/tla/CASES.tsv` (group `rnxdpreplay`) with measured records in
`proofs/tla/RUNS.tsv`.

## The design

- The map: `ROCKETNET_XDP_REPLAY_MAX_ENTRIES` (1024) entries, keyed by the
  source family, port and address, valued by a 32-bit `high_water`. The same accepted forget removes the learned reply MAC, including for a seen slot that never acquired a replay mark.
- The kernel rule, for the session packet types only (keep-alive, payload,
  disconnect, ack, fragment), after the type and size check and before the
  redirect: look the source up; with an entry, drop when
  `(int32_t)(high_water - sequence) >= ROCKETNET_XDP_REPLAY_WINDOW`,
  counting `ROCKETNET_SERVER_XDP_COUNTER_DROPPED_REPLAYED`; otherwise go on.
  The kernel never updates or deletes an entry, so a forged sequence far
  ahead passes to userspace, fails the AEAD open there, and moves nothing.
- Userspace, once per update after packet processing and before the transport
  flush: FORGET the old address of every slot that held a mark and is no
  longer connected (or whose mapping changed address); then MARK every
  connected slot whose `highest_opened` differs from what it last marked
  (none counting as zero), and every slot the walk first sees, zero
  included. The transport applies all forgets, then all marks, in one commit.
  For one session the map lags the truth by at most one update and is never
  ahead of it.
- A reconnect: a new session from the same address may arrive before the
  old session's forget is queued (the same slot, before any walk saw the
  release) or while the kernel refuses that delete. Its `highest_opened` is
  0 (it has sent nothing: a client sends no session packet before the
  server's first keep-alive), so the walk marks it at 0, as a fall below the
  slot's old mark or as a slot first seen, and that zero entry, which drops
  nothing, replaces the old session's entry in the commit. The server
  publishes the first keep-alive after that commit (the replay walk and
  commit come before the update's send flush in
  `sdk/rocketnet_server_transport.cpp`), so no packet of the new session
  meets the old entry, whether or not the old forget has landed.
- A refused forget: the kernel may refuse a delete (any error but a missing
  key). A refused batch delete is walked one element at a time so each
  forget's outcome is known, and every forget refused stays pending at the
  front of the buffer (`rocketnet_xdp_replay_marks_reset_keeping`), applied
  first in the next commit, ahead of that commit's marks. The stale entry
  then outlives the released slot only by the commits the forget took to
  land; a reconnect from the same address inside that time replaces it
  with its own zero mark first (above). The buffer's own refusals (a full
  buffer) are added to the socket's commit failure count.
- A refused mark: the kernel may refuse an update too. The walk's per-slot
  cache has already advanced, so it would never take that mark again; the
  pending buffer keeps every refused mark (`rocketnet_xdp_replay_marks_reset_keeping`,
  `mark_applied`) and the next commit applies it again, until it lands. A
  newer mark of the same key replaces the kept one in place, and a forget
  the walk queues for the key drops it: the commit applies forgets before
  marks, so a kept mark of a released slot would otherwise rewrite the
  address's entry after its forget. A mark the pending buffer cannot queue
  leaves the walk's cache where it was and the slot owes it (`mark_owed` in
  the walk's slot record), so the next walk asks again.
- A held greeting: a new session's first mark is a zero, and until the kernel
  has applied it the old session's entry may stand for the address. The
  socket therefore holds every keep-alive to that address at publish
  (`rocketnet_server_socket_hold_greetings`), the keep-alive being how the
  client learns it is connected; a zero mark the buffer could not queue
  names no key, so that publish holds every keep-alive. Only keep-alives
  are held: the client refuses payloads before connected, and a disconnect
  still leaves. The keep-alive is sent again when next due, after the retry
  lands, so the new session's first packet never meets the old entry.
- A superseded forget: a kept forget belongs to the session it was queued
  for. When a mark of the same key lands in a later commit while the kept
  forget is refused again, that entry is the newer session's, so the reset
  drops the stale forget instead of keeping it (`mark_applied` in
  `rocketnet_xdp_replay_marks_reset_keeping`; a refused batch update is
  walked one element at a time so each mark's outcome is known). Kept, the
  forget's later successful retry would delete the newer session's mark, and
  the walk, whose cache already holds that high-water, would not mark it
  again until a fresh sequence: replay-only traffic would bypass the kernel
  filter. A kept forget whose same-key mark was also refused stays kept.
- Sizing: the kernel window is the userspace window,
  `ROCKETNET_XDP_REPLAY_WINDOW` = `ROCKETNET_REPLAY_WINDOW_BITS`
  (`ROCKETNET_HISTORY_PACKETS`, 4096 by default; a static_assert ties them).
  A session's own mark is never ahead of its `highest_opened`, so the kernel
  drops only what the userspace window refuses: a packet reordered by less than a window is
  accepted on XDP exactly as on every other transport. The rule is one
  compare against the entry, so the window's size costs no map memory and no
  verifier work.

## What the module covers

`pendingForget` is a bounded set of source keys. It represents the fixed
userspace buffer after duplicate pending deletes coalesce: retrying one source
cannot consume the capacity required by another source.

Constants: `Addresses` (client source address and port), `MaxSeq` (the
highest sequence sent or forged), `W` (the userspace replay window depth,
which the design's kernel window equals), `Sessions` (the
sessions over the whole run, so a reconnect from one address is reachable),
`MaxInFlight` (the bound on packets in the network).

| Variable | Design | Meaning |
|---|---|---|
| `session[a]` | the slot's connection | the live session id, `0` for none |
| `sent[a]` | client send sequence | highest genuine sequence the live session sent |
| `opened[a]` | `highest_opened` of `replay_session` | raised only after a packet opens |
| `seen[a]`, `replyMac[a]` | slot identity and reply path | retained until its forget is accepted; a full pending buffer cannot let a new mapping overwrite the old identity |
| `marked[a]` | the transport's per-slot record | what it last marked, `0` for none |
| `kmap[a]` | `server_xdp_replay_map` entry | `hw` is `high_water`, `0` for no entry; `sess` is a ghost naming the session the mark was taken from |
| `net` | packets in flight | each `[addr, seq, session, genuine, stage, copy]`; `stage` is `"wire"` before the kernel, `"user"` after it passed; `copy` lets a duplicate coexist with its original |
| `pendingForget`, `pendingMark` | the walk's output | what the next commit applies |
| `phase` | the update | `"run"` while packets are processed, `"flush"` between the walk and the commit |
| `reached[s]` | (ghost) | the highest `highest_opened` session `s` reached |
| `dropped` | (ghost) | genuine packets the kernel dropped, with the session of the mark it dropped them against |
| `forgetFailures` | (bound) | the forgets the kernel refused so far, at most `ForgetFailureBudget` (1; the controls raise it to 2, the superseded-forget witness to 3) |
| `markFailures` | (bound) | the marks refused so far by the kernel or by the pending buffer, at most `MarkFailureBudget` (0; the refused-mark control raises it to 2, its witnesses to 1) |
| `markOwed[a]` | the walk's `mark_owed` | the buffer refused the walk's mark, so the walk takes it again |
| `greeted[a]` | the client's connected state | the live session's first keep-alive has left: set by the `Commit` after its `Connect`, cleared by `Connect` and `Release` |
| `nextSession` | (ghost) | the id the next connection takes |

Eleven rule switches are definitions with the design's values, each
overridden by one witness configuration: `KernelAdvances` (`FALSE`),
`WalkForgets` (`TRUE`), `WalkForgetsUnmarked` (`TRUE`), `KernelWindow` (`W`),
`KeepFailedForgets` (`TRUE`), `SupersedeStaleForgets` (`TRUE`) and
`MarkNewSessions` (`TRUE`), `KeepRefusedMarks` (`TRUE`),
`ForgetDropsKeptMark` (`TRUE`), `HoldGreetingUntilMarked` (`TRUE`) and
`AdvanceOnQueueRefusal` (`FALSE`).

Actions, each against the design sentence it models:

- `Connect(a)`: a new session; sequences restart at 1 and `highest_opened` is
  0. It may reconnect while an older delete is still owed (not yet queued,
  or refused): there is no settle guard. The session is not greeted.
- `ClientSend(a)`: the live session sends its next genuine sequence, once it
  is greeted: the client sends session packets only once the server's first
  keep-alive has reached it.
- `Forge(a, s)`: an attacker spoofing `a` sends any sequence `s`; it never
  opens.
- `Duplicate(p)`: the network delivers a second copy of a packet.
- `KernelFilter(p)`: THE KERNEL RULE. With an entry and
  `hw - seq >= KernelWindow`, the
  packet is dropped (a genuine one is recorded in `dropped`); otherwise it
  passes to userspace. The design leaves `kmap` unchanged; the broken shape
  writes `kmap[a] := seq` on every pass. The kernel runs beside the update,
  so this is enabled in both phases.
- `UserOpen(p)`: a genuine packet of the live session opens and raises
  `highest_opened`; a forged one fails the AEAD open, and a packet of an
  ended session has no mapping: neither moves anything.
- `Release(a)`: the session ends.
- `Walk`: forgets first (every seen address with no live session), then
  marks (every live session with `opened[a] # marked[a]`, or not yet seen,
  zero included; the broken shape `MarkNewSessions = FALSE` marks only
  `opened[a] # 0`), into the pending sets; `marked` records them.
- `Commit`: the transport applies all forgets, then all marks, to `kmap`. The
  only action that writes `kmap` in the design. The kernel may refuse any set
  of the pending forgets, within `ForgetFailureBudget` over the run; a refused
  forget leaves its entry and stays pending (the walk adds to the pending set
  rather than replacing it), so the next commit applies it before its marks,
  unless a mark of the same address was applied in the same commit: then the
  forget is superseded and dropped; only an applied mark supersedes. The
  kernel may refuse any set of the marks too, within `MarkFailureBudget`: a
  refused mark leaves the entry as it was and stays pending; the walk adds
  its marks to the kept ones, a newer mark of an address replacing the kept
  one, and an accepted forget dropping a kept mark of its address. A mark of
  0 is an entry that drops nothing. The update's send flush follows the
  commit and greets every live session except one whose zero mark is kept
  after a refusal or owed by the walk (`HoldGreetingUntilMarked`); a greeted
  session stays greeted. The walk may find the buffer full for a mark of an
  address with no kept mark: it then leaves `marked` and sets `markOwed`.
  The broken shapes drop every refused forget, keep one over the newer mark,
  drop every refused mark, keep a mark over the forget of its address,
  greet over a refused zero mark, or advance the cache on a refused queue.

`Connect`, `UserOpen`, `Release` and `Walk` are userspace steps of one
update, so they run only in phase `"run"`; `Commit` ends the flush.

Properties:

- `TypeOK`.
- `KernelNeverWrites`, an action property: every step but `Commit` leaves
  `kmap` unchanged.
- `ReleasedSlotForgetsItsReplyMac`: an accepted forget removes the learned reply MAC, including for a released slot that was seen before it acquired a mark.
- `NeverSeenNeverSentTo`: a reply with no frame in hand goes only to an
  address a frame came from; to any other it is refused.
- `HeldReplyFindsItsMac`: a held reply, or its duplicate, to an address a
  frame came from is never refused for want of a MAC, before the learn (a
  handshake reply) or after the forget (a reply held for a released client).
- `AnsweredOnlyHeard`: an answered entry exists only for an address a frame
  came from.
- `ForgetOwedUntilApplied`: an entry for an address with no live session is
  owed a forget: the walk has yet to take it (`marked[a] # 0`, or `seen[a]`:
  the walk forgets every seen released slot, and a refused zero mark leaves
  an older entry under a seen slot whose cache reads 0) or it is
  pending, kept across every commit the kernel refused it. No stale entry
  outlives a released slot by more than the commits it took to succeed.
- `MarkNeverAhead`: with a live session, the entry for its address is none,
  or was taken from that session and is at most its `highest_opened`, or a
  mark of that session is pending (for this commit, or kept after a refusal),
  or the session is not yet greeted and the walk still owes it its first
  mark. With no live
  session the forget is owed (`ForgetOwedUntilApplied`, a conjunct).
- `RetryNeverDeletesNewerMark`: no pending forget is held over the live
  session's own entry at its address, so no retry ever deletes a newer
  session's mark.
- `MarksTakenFromTheTruth`: every entry is at most the `highest_opened` its
  session reached.
- `FreshGenuineNeverDropped`: a genuine packet of the live session dropped
  against a mark of its own session is never fresh: its sequence is at most
  `opened[a] - W`, so the userspace window, `W` deep, would refuse it too.
- `NewSessionNeverDroppedByOldMark`: no genuine packet is dropped against a
  mark taken in an earlier session, whether or not that session's forget had
  been queued or accepted, or whether the kernel or the buffer refused the
  new session's first mark. All three controls check it.
- `MarkOwedUntilApplied`: a greeted live session's entry is the walk's last
  mark for it (an entry of 0 drops nothing, whichever session took it), or
  that mark is pending, kept across every commit the kernel refused it: a
  refused mark is retried until it is applied.

The last two split one statement by the session of the mark: together they
say no genuine packet of the live session within `W` of its last opened
sequence is dropped in the kernel, and each reversed witness reaches exactly
one of them.

`UserOpen` opens every genuine packet of the live session, inside the
userspace window or not: a genuine packet the window would refuse is at or
below `highest_opened`, so refusing it and opening it leave the same state.

## Reconnect freshness

A reconnect is allowed before the old delete is queued or lands, with no
settle guard on `Connect` and no trace suppression. The new session's first
mark, zero included, replaces the old entry in the commit that greets it, so
`MarkNeverAhead` and `NewSessionNeverDroppedByOldMark` hold in both controls.
`MCRNXdpReplayBrokenReconnectFreshness.cfg` restores the old walk rule (a
nonzero `highest_opened` only, `MarkNewSessions <- MarkNewSessionsBroken`)
and fails `NewSessionNeverDroppedByOldMark`.

## Held replies and the reply MAC

With no gateway the server addresses a reply by the client's learned MAC
(`replyMac[a]`, learned at `Connect`) or, for a direct response, by the
source MAC of the frame it answers. The development link simulator holds a
reply for its latency, may send it twice, and the transport looks the MAC up
only when the simulator lets it go, after the receive view it answered is
gone. Measured on the XDP bench before this rule: every held cookie reply
was refused (`SEND_NO_LINK_ADDRESS`), so no client connected on an impaired
link, and an ack still held for a released client was refused after its
forget.

The rule (`sdk/rocketnet_xdp_frame.h`): a direct response keeps its frame's
MAC as the address's answered entry (`Answer`, `answered[a]`); a reply with
no frame in hand takes the learned MAC, else its own address's answered
entry, else it is refused (`Drain`); a learn takes the answered entry's slot
(`Connect`), and a forget leaves the learned MAC as the answered entry
(`Commit`). `replyMac[a]`, the learned MAC, is still dropped by the forget, so
`ReleasedSlotForgetsItsReplyMac` is unchanged. `HoldSessionReply` holds a live
session's reply and `HoldStray` a reply to any address, one no frame need
ever have come from. `HeldReplyBudget` bounds the held replies over a run; it
is 0 in every control but `MCRNXdpReplayHeldReply.cfg`, so those controls
reach exactly the states they did before.

The code marks a send addressed by an answered entry `mac_learned` (its
pointer is inside the table), so the socket's finish does not refresh it
from a learn made between its start and its finish. Such a send has no frame
in hand, so it is started and finished outside receive processing, where no
learn runs. Measured with a probe on a 120 s impaired soak in the guest: 13
sends addressed by an answered entry, 0 with a learn of their address
between start and finish.

## Left out

- **The 32-bit wrap.** Sequences are naturals and `hw - seq` is integer
  subtraction; the code's `(int32_t)(high_water - sequence)` matches it while
  the two are within 2^31.
- **The byte layout.** The key (family, pad, port, address) and value
  (`high_water`, pad) structs in `sdk/rocketnet_wire.h`, the little-endian
  sequence after the 6-byte prefilter header, and the packet type list are
  not modelled; an address in `Addresses` stands for a key.
- **Map capacity.** `ROCKETNET_XDP_REPLAY_MAX_ENTRIES` is not modelled; a
  full map refusing a mark is one of the refused marks above.
- **Slots and addresses.** A slot is identified with its address. A slot
  whose mapping changes address forgets the old address exactly as a release
  does; a reconnect landing in a different slot is a reconnect here (the
  walk sees that slot first, `~seen[a]` after the old slot's forget was
  queued, and the fake-map regression covers both slots).
- **A disconnect before the first keep-alive.** A client that gives up while
  still sending its connection response sends a disconnect burst, session
  packets, before it is greeted; the kernel may drop that burst against an
  old entry, and the server then times the slot out instead of ending it at
  once. No session is lost, so `ClientSend` is enabled only once greeted.
- **Addresses are independent.** Every action touches one address. The
  retention control uses two addresses and one pending-forget entry, so it
  reaches a full buffer while another unique delete remains owed.
- **The reply MAC's value, the gateway and the table's size.** A MAC is the
  client's own one, so the model names whether a MAC is known, not which; a
  spoofed frame's MAC, which the newest answer would take as a direct
  response does, is left out, as are the gateway (with one, every reply to an
  unlearned address goes to it), the probe window and the answered entries a
  full window cannot keep.
- **The userspace window's bits.** Only its depth `W` enters, through
  `FreshGenuineNeverDropped`; a genuine packet that reaches userspace opens
  to the same maximum whether the window would accept or refuse it.

## Running

From the Rocketnet checkout, with the pinned runner and JAR of
`proofs/tla/README.md`:

```sh
for i in $(seq 0 17); do
  "$job/tlacheck" run --root . --jar "$jar" --java "$java" \
    --group rnxdpreplay --shards 18 --shard "$i" --workers 2 --timeout 110s \
    --dir "$job/rnxdpreplay-$i"
done
"$job/tlacheck" merge --root . --keep proofs/tla/RUNS.tsv \
  --out "$job/RUNS.tsv" "$job"/rnxdpreplay-*/RUNS.tsv
"$job/tlacheck" groups --root . --stale
```

## The small instance

`MCRNXdpReplay.cfg`: two addresses, `MaxSeq = 3`, `W = 2`, `Sessions = 2`,
`MaxInFlight = 1`, `ForgetCapacity = 1`. TLC explores every send, forgery,
duplicate, kernel pass or drop, open, release, walk and commit interleaving,
with up to two refused forgets anywhere in the run
(`ForgetFailureBudget <- ForgetFailureBudgetTwo`, so a kept forget can be
refused again in the commit that lands a newer session's mark): 239,206
distinct states (596,195 generated, depth 36), and `TypeOK`,
`ForgetOwedUntilApplied`, `MarkNeverAhead`, `MarksTakenFromTheTruth`,
`FreshGenuineNeverDropped`, `NewSessionNeverDroppedByOldMark`,
`ReleasedSlotForgetsItsReplyMac`, `RetryNeverDeletesNewerMark` and
`KernelNeverWrites` hold. The measured records are in `proofs/tla/RUNS.tsv`.

`MCRNXdpReplayReconnect.cfg`: the same, with `Sessions = 3`, so a reconnect
at one address can meet a release, a full pending buffer or a refused
delete at the other: 4,527,982 distinct states (11,919,449 generated, depth
49), every property of the control holding.

`MCRNXdpReplayRefusedMark.cfg`: two addresses, `MaxSeq = 3`, `W = 2`,
`Sessions = 2`, `MaxInFlight = 1`, `ForgetCapacity = 1`, one refused forget
and up to two refused marks, by the kernel or by the buffer
(`MarkFailureBudget <- MarkFailureBudgetTwo`), so a reconnect's zero mark
can be refused, kept or owed, refused again and applied, with its greeting
held throughout. `NewSessionNeverDroppedByOldMark`, `MarkOwedUntilApplied`,
`MarkNeverAhead`, `ForgetOwedUntilApplied`, `RetryNeverDeletesNewerMark` and
the rest hold. The measured records are in `proofs/tla/RUNS.tsv`.

`MCRNXdpReplayHeldReply.cfg`: two addresses, `MaxSeq = 3`, `W = 2`,
`Sessions = 2`, `MaxInFlight = 1`, `ForgetCapacity = 1`, and up to two held
replies (`HeldReplyBudget <- HeldReplyBudgetTwo`): a handshake reply, a live
session's reply or a reply to an address no frame came from, each sent later
with no frame in hand, before or after a learn, a release and its forget.
`NeverSeenNeverSentTo`, `HeldReplyFindsItsMac`, `AnsweredOnlyHeard` and
every property of the design control hold. The measured records are in
`proofs/tla/RUNS.tsv`.

## Reversed witnesses

| Configuration | Rule removed | Expected failure |
|---|---|---|
| `MCRNXdpReplayBrokenKernelAdvances.cfg` | the kernel never writes: `KernelAdvances <- KernelAdvancesBroken`, `kmap[a] := seq` on every pass | `FreshGenuineNeverDropped`, exit 12, depth 6 |
| `MCRNXdpReplayBrokenNoForget.cfg` | the walk forgets a released slot: `WalkForgets <- WalkForgetsBroken` | `ReleasedSlotForgetsItsReplyMac`, exit 12, a 6-state trace |
| `MCRNXdpReplayBrokenNarrowWindow.cfg` | the kernel window is the userspace window: `KernelWindow <- KernelWindowBroken` (`W - 1`) | `FreshGenuineNeverDropped`, exit 12, depth 9 |
| `MCRNXdpReplayBrokenUnmarkedNotForgotten.cfg` | forgetting only marked slots | `ReleasedSlotForgetsItsReplyMac`, exit 12, depth 6 |
| `MCRNXdpReplayBrokenFailedForgetDropped.cfg` | a refused forget stays pending: `KeepFailedForgets <- KeepFailedForgetsBroken` | `ForgetOwedUntilApplied`, exit 12, depth 10 |
| `MCRNXdpReplayBrokenSupersededForget.cfg` | a superseded forget is dropped: `SupersedeStaleForgets <- SupersedeStaleForgetsBroken` | `RetryNeverDeletesNewerMark`, exit 12, a 15-state trace |
| `MCRNXdpReplayBrokenReconnectFreshness.cfg` | a new session is marked at zero: `MarkNewSessions <- MarkNewSessionsBroken` | `NewSessionNeverDroppedByOldMark`, exit 12, a 19-state trace |
| `MCRNXdpReplayBrokenRefusedMarkDropped.cfg` | a refused mark stays pending: `KeepRefusedMarks <- KeepRefusedMarksBroken` | `MarkOwedUntilApplied`, exit 12 |
| `MCRNXdpReplayBrokenKeptMarkAfterForget.cfg` | a queued forget drops a kept mark of its address: `ForgetDropsKeptMark <- ForgetDropsKeptMarkBroken` | `ForgetOwedUntilApplied`, exit 12 |
| `MCRNXdpReplayBrokenGreetBeforeMark.cfg` | the greeting waits for the zero mark: `HoldGreetingUntilMarked <- HoldGreetingUntilMarkedBroken` | `NewSessionNeverDroppedByOldMark`, exit 12, a 21-state trace |
| `MCRNXdpReplayBrokenQueueRefusedAdvances.cfg` | a refused queue leaves the cache: `AdvanceOnQueueRefusal <- AdvanceOnQueueRefusalBroken` | `MarkOwedUntilApplied`, exit 12, a 9-state trace |
| `MCRNXdpReplayBrokenAnsweredNotKept.cfg` | a direct response keeps its answered entry: `KeepAnswered <- KeepAnsweredBroken` | `HeldReplyFindsItsMac`, exit 12, a 3-state trace |
| `MCRNXdpReplayBrokenForgetDropsAnswered.cfg` | a forget leaves the answered entry: `ForgetLeavesAnswered <- ForgetLeavesAnsweredBroken` | `HeldReplyFindsItsMac`, exit 12, a 9-state trace |
| `MCRNXdpReplayBrokenAnswerAnyAddress.cfg` | a reply takes its own address's answered entry: `AnswerKeyedByAddress <- AnswerKeyedByAddressBroken` | `NeverSeenNeverSentTo`, exit 12, a 4-state trace |

Each witness configuration leaves out the properties that restate the rule it
removes, so the run reaches the harm rather than the rule itself. Run with
every property of the control, the same overrides fail one step after the
broken rule acts: the kernel-advances shape fails `MarksTakenFromTheTruth` at
depth 3 (a forged packet passes and its sequence becomes the entry), and the
no-forget shape fails `MarkNeverAhead` at depth 11 (the reconnect, with the
old session's entry still in the map; with this design's zero mark that
reconnect is now covered, and the shape fails `ReleasedSlotForgetsItsReplyMac`
instead). Both were bench experiments, not cases.

### KernelAdvances, checked by hand against the design

1. `Connect(A1)`: session 1; `ClientSend(A1)` puts genuine sequence 1 on the
   wire.
2. `Forge(A1, 3)`: a forged sequence 3 on the wire.
3. `KernelFilter` on the forgery: no entry, so it passes, and the broken
   kernel writes `high_water = 3`.
4. `KernelFilter` on genuine 1: `3 - 1 >= 2`, dropped, with `highest_opened`
   still 0.

In the design the kernel never writes at step 3; the forgery reaches
userspace, fails the open, and the entry stays absent, so genuine 1 passes.
At the real window the forgery need only be a window ahead of the genuine
traffic: one spoofed packet would silence a client.

### NoForget, checked by hand against the design

1. `Connect(A1)`: session 1, its reply MAC learned; `Walk` sees the slot;
   `Commit`.
2. `Release(A1)`; `Walk`: the broken walk forgets nothing, so the flush
   phase holds the released address's reply MAC with no MAC forget pending.

In the design the walk queues `A1` with its MAC forget. The kernel entry
the broken walk leaves no longer harms a reconnect (the new session's zero
mark replaces it), so `NewSessionNeverDroppedByOldMark` holds here; the
loss is the reply MAC and the map space a stale entry keeps.

### ReconnectFreshness, checked by hand against the design

The TLC trace, 19 states:

1. `Connect(A1)`: session 1; `Walk`, `Commit` greet it; it sends 1, 2, 3,
   all opened (`highest_opened = 3`).
2. `Walk` marks 3; `Commit` writes `high_water = 3`.
3. `Release(A1)`, then `Connect(A1)`: session 2 before any walk saw the
   release, so the slot is still seen at `A1` with `marked = 3`.
4. `Walk`: no forget (the slot is connected), and the broken walk takes no
   mark (nothing opened). `Commit` greets session 2 with session 1's entry.
5. Session 2 sends 1; `KernelFilter`: `3 - 1 >= 2`, dropped against session
   1's mark.

In the code this was the walk's `highest_opened == 0` skip in
`rocketnet_server_replay_marks_walk`: the same slot reconnected between two
walks matched its seen address and was never forgotten or re-marked, and a
reconnect into another slot met a forget the kernel refused. The design
marks session 2 at 0 at step 4. The fake-map regression is
`test_xdp_transport_replay_marks_reconnect_admits_the_new_session`.

### NarrowWindow, checked by hand against the design

1. `Connect(A1)`: session 1 sends 1 and 2; the kernel passes 2 (no entry),
   and userspace opens it (`highest_opened = 2`); 1 is still on the wire,
   reordered behind 2.
2. `Walk` marks 2; `Commit` writes `high_water = 2`.
3. `KernelFilter` on genuine 1: the broken window is `W - 1 = 1`, and
   `2 - 1 >= 1`, so it is dropped, though `1 > opened - W = 0`: the userspace
   window, 2 deep, would have accepted it.

In the design the kernel window is `W`, `2 - 1 < 2`, and sequence 1 passes to
userspace. This is the shape of the old `ROCKETNET_XDP_REPLAY_WINDOW` of 16
against a userspace window of 4096: on XDP alone, a packet reordered 16 to
4095 behind was dropped in the kernel as replayed.

### FailedForgetDropped, checked by hand against the design

1. `Connect(A1)`: session 1 sends 1; the kernel passes it and userspace
   opens it (`highest_opened = 1`).
2. `Walk` marks 1; `Commit` writes `high_water = 1`.
3. `Release(A1)`; `Walk` forgets `A1`; `Commit`, with the kernel refusing
   the forget: the entry stays, and the broken commit drops the forget, so
   `A1` is neither marked nor pending.

In the design the refused forget stays pending and the next commit applies
it first. In the code this was `bpf_map_delete_batch` failing with an error
other than a missing key or an older kernel's refusal, or one
`bpf_map_delete_elem` failing: the failure was counted and the forget lost,
the walk having already cleared the slot's mark record, so a reconnect from
the same address and port had its session packets dropped in the kernel as
replayed until it timed out.

### RefusedMarkDropped, checked by hand against the design

The TLC trace, 9 states:

1. `Connect(A1)`: session 1; `Walk` marks it at 0 (first seen); `Commit`.
2. Session 1 sends 1, opened (`highest_opened = 1`); `Walk` marks 1, and
   the walk's cache reads 1.
3. `Commit`: the kernel refuses the mark and the broken commit drops it.
   The entry is still `[hw 0, sess 1]` while the cache reads 1, and nothing
   takes the mark again until a fresh sequence opens.

In the design step 3 keeps the mark and the next commit applies it. In the
code this was `rocketnet_xdp_replay_marks_reset_keeping` clearing every
mark: a reconnect's refused zero mark left the old session's entry, and the
walk, already at 0 for the slot, never asked again. The fake-map regression
is `test_xdp_transport_replay_marks_refused_reconnect_mark_is_retried`.

### KeptMarkAfterForget, checked by hand against the design

The TLC trace, 12 states:

1. `Connect(A1)`: session 1, marked at 0 and greeted; it sends 1, opened;
   `Walk` marks 1.
2. `Commit`: the kernel refuses the mark, which is kept.
3. `Release(A1)`; `Walk` queues the forget of `A1`, and the broken walk
   leaves the kept mark in place.
4. `Commit`: the forget lands, then the kept mark of the released session
   writes `[hw 1, sess 1]`: an entry for a released address with no forget
   owed.

In the design step 3 drops the kept mark (`rocketnet_xdp_replay_marks_forget`
drops a kept mark of the key it queues), so the forget is the last word.

### GreetBeforeMark, checked by hand against the design

The TLC trace, 21 states:

1. Session 1 at `A1` opens to 3; `Walk` marks 3; `Commit` writes
   `[hw 3, sess 1]`.
2. `Release(A1)`, then `Connect(A1)` as session 2 before any walk; `Walk`
   takes its zero mark.
3. `Commit`: the kernel refuses the zero mark, which is kept, and the broken
   commit greets session 2 anyway.
4. Session 2 sends 1; `KernelFilter`: `3 - 1 >= 2`, dropped against session
   1's mark.

In the design step 3 holds the greeting: session 2 is greeted only by the
commit that applies its zero mark, so its sequence 1 meets that entry. In
the code this was the keep-alive leaving at publish whatever the commit did;
`rocketnet_server_socket_hold_greetings` now drops it while the zero mark is
kept, and the regression is
`test_xdp_transport_replay_marks_refused_reconnect_mark_is_retried` with the
publish case in `rocketnet_server_socket_test_replay_marks`.

### QueueRefusedAdvances, checked by hand against the design

The TLC trace, 9 states:

1. Session 1 at `A1`, marked at 0 and greeted; it sends 1, opened.
2. `Walk`: the buffer refuses the mark of 1, and the broken walk advances
   its cache to 1 anyway; nothing is pending.
3. `Commit`: the entry stays `[hw 0, sess 1]` behind a cache of 1, and no
   later walk asks for that mark again.

In the design the walk leaves `marked` at 0 and owes the mark, and the next
walk takes it. In the code `sink->mark` returned nothing and the walk
advanced unconditionally; it now returns false on a full buffer and the slot
records `mark_owed` (`test_server_replay_marks_refused_mark_is_owed`).

### SupersededForget, checked by hand against the design

The TLC trace, 15 states:

1. `Connect(A1)`: session 1; `Walk` marks it at 0 (first seen); `Commit`.
2. `Release(A1)`; `Walk` forgets `A1` (a seen slot is forgotten marked or
   not); `Commit` with the kernel refusing the forget (the first refusal):
   the forget stays pending.
3. `Connect(A1)`: session 2; `Walk` marks it at 0; `Commit` refuses the
   kept forget again (the second refusal) and greets session 2.
4. Session 2 sends 1, opened; `Walk` marks 1 for session 2.
5. `Commit`: the kept forget is refused a third time and the mark lands,
   `kmap[A1] = [hw 1, sess 2]`. The broken commit keeps the forget, so `A1`
   is pending over the live session's entry.

The same holds with an old entry for session 1 in place; TLC reports the
shortest trace.

In the design step 4 drops the superseded forget, and the next successful
commit has nothing to delete. In the code this was
`rocketnet_xdp_replay_marks_reset_keeping` keeping every refused forget: the
next successful delete removed session 2's mark, and the walk's cached
high-water kept it from being marked again until a fresh sequence opened.
The fake-map regression is
`test_xdp_transport_replay_marks_stale_forget_never_deletes_a_newer_mark`.

### AnsweredNotKept, checked by hand against the design

The TLC trace, 3 states: `Answer(A1)`, a handshake frame from `A1` with no
session, answered by a direct response the link holds; `Drain(A1)`: the
reply leaves with no frame in hand, nothing is learned and no answered entry
was kept, so it is refused. This is the table before the answered entry, and
the measured failure: on the XDP bench the cookie reply (38 bytes) was
reserved inside the receive view with the request's MAC, held by the link
simulator, and refused when it left 30 to 50 ms later, 93 of 94, with no
learn ever made, since the client never received a cookie. In the design the
answer keeps `answered[A1]` and the drain sends to it. The regression is
`test_xdp_transport_reply_mac_held_reply_finds_the_answered_mac`.

### ForgetDropsAnswered, checked by hand against the design

The TLC trace, 9 states: `Connect(A1)`, `Walk`, `Commit`; `Release(A1)`;
`Walk` queues the forget; `Commit` applies it and drops the learned MAC with
no answered entry left; `HoldStray(A1)` and `Drain(A1)`: a reply held for the
released client leaves and is refused. This is the measured failure of the
first answered-entry table: an ack (765 bytes) the link simulator held for a
client released at a reconnect was refused after its forget, one refusal in
a 120 s impaired soak. In the design the forget leaves `answered[A1]`, and
the drain sends to it.

### AnswerAnyAddress, checked by hand against the design

The TLC trace, 4 states: `Answer(A1)` keeps `answered[A1]`; `HoldStray(A2)`,
a reply to an address no frame came from; `Drain(A2)` takes `A1`'s answered
MAC and sends. The design looks the answered entry up by the reply's own
address (`rocketnet_xdp_reply_macs_find_state`), so the drain to `A2` finds
none and is refused.
