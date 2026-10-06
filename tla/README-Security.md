# Security: tokens, the cookie gate, the tables, the keys, the replay window, with an attacker

`tla/Security.tla` is the security half of connection setup in
`sdk/rocketnet_server_protocol.cpp` (the request ladder, the response ladder, the sealed
session ladder), the two token tables of `sdk/rocketnet_token_tables.cpp`, the replay
window of `sdk/rocketnet_replay.cpp` at the representation level, and the per-mapping
key exchange, as STANDARD.md specifies them (_Keys_, _The Prefilter Header_, _Connect
Token_, _Connect token sharing_, _Per-connection keys_, _The Handshake_, _Replay
Protection_, _Processing Connection Requests_, _Processing Connection Response
Packets_). An attacker sits on the path. Every packet crosses the shared lossy link
`tla/Link.tla`, one instance per direction. `tla/MCSecurity.tla` and `tla/MCSecurity.cfg`
are the small instance; every TLC run is a row in `tla/RUNS.tsv`. The inventory this
card worked from is `docs/TLA-INVENTORY.md`, M2, M5, M6 and M7 (the module list's
`RNReplayWindow`, `RNSecurityTables` and the security half of `RNHandshake`).

## What the model covers

**The token lifecycle.** A token is a name with four attributes given by the instance:
`Signed` (the backend's signature verifies), `ForThisServer` (sealed to this server's
key), `Cid` (the client id in the sealed block) and `Expire` (the expire timestamp;
0 = never). The small instance has `"th"`, the honest client's; `"tx"`, genuine but
sealed to another server; `"tf"`, carrying no valid signature; `"te"`, genuine and
already expired at clock 1. _Issued_ is the instance; _presented_ is a request on the
wire; _admitted_ is the slot assignment at the response (the ghost `admits[t]` counts
it); _expired_ is `ExpTime(t) <= clock`; _replayed_ is the attacker's `InjectRequest`
with a captured token or `ReplaySealed` with a captured sealed packet.

**The prefilter.** Epoch 0 is the whole of 1.0 and its constant is public, so the
attacker can always pass it; what the filter buys is that packets that are not this
title's (`Junk`) are dropped before any state is touched, whichever filter value they
carry. The keyed form is not implemented by 1.0 (STANDARD.md, _The Prefilter Header_)
and has nothing to check here beyond the same statement: `JunkTouchesNothing`, and
`NothingFromUnkeyed`, which says every mapping and slot rests on a signed token sealed
to this server presented from an address whose cookie verified.

**The cookie gate** (`rocketnet_server_protocol_process_connection_request`, the block
after the fixed-offset checks). A cookie is `[addr, exp, gen]`; the MAC is the
attacker's inability to make one. The gate runs in the code's order: zero field, then
the MAC under the current key and (when `AcceptPrevious` and a previous key is held)
the previous one, then expiry; a failure answers a fresh cookie under the current key
with `exp = clock + CookieLife`. The server stores nothing for it, which is why
`UnauthStateBounded` has no cookie term. `Rotation` and `RotatePeriod` model the
rotation the standard asks for and the code does not do (F2).

**The used-token history and the admitted-token set** (`rocketnet_token_tables.cpp`).
`history[t]` is `None` or `[addr, exp, order]`; `admitted[t]` is 0 or the expire time.
An expired entry reads as absent (`rocketnet_token_table_find`). `HistoryRecord` is
`rocketnet_token_table_select_slot` with `evict_oldest = 1`: a free slot, then an
expired one, then the oldest. `AdmittedAfterAdd` is the set's add: present means
success (F9), then a free or expired slot, else refuse, never evicting an unexpired
entry. The request ladder consults `check` and `contains` and then records, in the
code's order; the response ladder checks slots before the set takes its entry.

**The key exchange.** Keys are the pair `[tok, nonce]`: the token whose keys the
derivation is keyed with, and the session nonce the server draws when it creates the
mapping (`nonceCtr`, never reused). A sealed packet opens iff the receiver holds the
pair. The honest client holds `HonestToken`'s keys and commits the nonce the challenge
carries; the server holds its mapping's pair; nobody else holds anything, so the
attacker can replay a sealed packet and never make one. The salt is per attempt and
the honest client makes one attempt, so it is not a variable. The ephemeral pair is
inside the derivation and not separately modelled; the client connection module
carries its erasure (F4).

**The replay window** at the representation level: `Words` words of `WordBits` bits in
a ring, `W = Words * WordBits`, with `hi` beside it. `AlreadyReceived` is
`rocketnet_replay_already_received`; `Advanced` is `rocketnet_replay_advance`, including
the branch that zeroes `ClearWords` words on a skip of `W` or more. The code has 64
words and clears 4 (`sdk/rocketnet_replay.cpp:54-60`); the instance has 2 words and the
run that reproduces F1 clears 1. A ghost `opened` per mapping records what actually
opened, so `InWindowExact` can compare the representation with the truth.

**The honest client** at address `HonestAddr`, as far as the security machines see it:
states 0, 1, 2, 3, 4, -1 and -7 of `sdk/rocketnet_client_protocol.cpp`; a request every
tick in 1 and 2 (with the stored cookie in 2), the response every tick in 3, a
keep-alive with the next sequence every tick in 4 up to `MaxSeq`; a cookie stored in 1
or 2 (replacing); a challenge accepted in 2 under its own token's keys; a denied under
its own token's keys in 2 or 3; the first keep-alive under the committed keys in 3.

**The server**: `map[a]` per address (`None`, or a half-open or established mapping
with its token, nonce, deadline, last-received time and replay window), `slots` (client
id, token, mapping address, confirmed), `others` (slots held by clients outside the
model), the two tables, the nonce counter, the cookie key generation, and the keep-alive
timer `sdue`. The ladders are the code's, in the code's order, one `IF` rung per
check: the connected-address and half-open rungs before anything is read; version,
protocol id, expire, server key; the cookie gate; the history check; the signature and
the seal; client id connected and token admitted; the record; the nonce; no slots (a
denied with no mapping); mapping table full; the mapping and its challenge. The
response ladder: mapping, open, already connected (the last-received time refreshed,
F7), client id connected, no slots or set full (denied, mapping kept), admit. The
session ladder for a client keep-alive: mapping, replay check, open, advance, not
connected or accepted (last-received refreshed, slot confirmed). The establishing
keep-alive is re-sent on the timer while the slot is unconfirmed; `RepeatKeepAlive =
FALSE` is the witness that sends it once. This is the model's rule, not the code's
exactly. `rocketnet_server_protocol_write_keep_alive_if_due`
(`sdk/rocketnet_server_protocol.cpp:1239-1316`) never stops sending. Until the client
has acknowledged a keep-alive (`keep_alive_acked`, latched at `sdk/rocketnet_ack.cpp:385`
when an ack block names one), it sends on the keep-alive's own clock, and other traffic
does not suppress that. After the ack it sends whenever nothing else went out in the
interval (`last_send_time`). The code's comment at `:1284-1294` rejects the confirmed
flag as the test. The model has no ack blocks and no other server traffic, so the
slot's confirmation stands in for the ack. Only a client keep-alive confirms, and the
honest client sends one only in 4, after a keep-alive has reached it, which is the
earliest point at which the code's latch can be set. The model then stops sending,
because nothing in it waits on a later keep-alive. So the model sends no more
keep-alives than the code, and possibly fewer.

**The attacker** (`Attacker = TRUE`): sees every packet handed to either link (the
latest cookie per address, every sealed client packet without its source); may send a
request from either address with any token and any cookie it has seen, none, or a
forgery (`InjectRequest`: the replay of a captured request, the expired and foreign
tokens, the flood); may send junk with the right or the wrong filter value
(`InjectJunk`); may re-send any sealed packet it has seen from either address
(`ReplaySealed`: the F7 refresh, the replay-window probe). Replay of a packet in
flight toward either peer is Link's `Duplicate`. The flood is `InjectRequest` and
`InjectJunk` enabled at every step up to the link's `Capacity`; the state it can grow
is what `UnauthStateBounded` bounds.

**Actions**, one per operation and per outside event: `Connect`, `ClientSend`,
`ClientRecvCookie`, `ClientRecvChallenge`, `ClientRecvDenied`, `ClientRecvKeepAlive`,
`OtherRecv` (a server packet addressed to the attacker leaves the link);
`ServerRecvRequest`, `ServerRecvJunk`, `ServerRecvResponse`, `ServerRecvKeepAlive`,
`ServerKeepAlive`; `OtherJoins`, `OtherLeaves`; `InjectRequest`, `InjectJunk`,
`ReplaySealed`; `Tick` (the clock, both links age, the handshake deadline and the idle
timeout sweep, the cookie key rotates on its period, both senders' timers fire);
`C2SEnvironment` and `S2CEnvironment` (Link's `Drop`, `Duplicate`, `Reorder`, `Delay`).
`KindLoss = "firstkind"` is the terrain the request named that Link cannot express
without reading packets: each sender's first send of each type is lost before it
reaches the link.

### Properties

Safety (`SafetyInvariants` in the committed run; needs no fairness):

| Property | Says | Source |
|---|---|---|
| `TokenAdmitsAtMostOnce`, `OneConnectionPerToken` | `admits[t] <= 1`; no two slots hold one token | S "a token admits one connection" |
| `NeverAcceptsReplay` | no session packet whose sequence was already opened was accepted | S _Replay Protection_ |
| `BeyondWindowRefused` | every sequence at or below `hi - W` reads as received | `rocketnet_replay_already_received` |
| `NothingFromUnkeyed` | every mapping and slot rests on a signed token sealed to this server from a cookie-proven address | S _Processing Connection Requests_ |
| `UnauthStateBounded` | mappings, history, set and slots never exceed their capacities; the cookie gate stores nothing | S _Half-open state_ |
| `NonceFresh` | no two live mappings share a nonce; every nonce was drawn | S _Per-connection keys_ |
| `SlotMappingBijection` | a slot is held only by an established mapping, one per mapping, one per client id | `rocketnet_client_slots` |
| `AdmittedNeverEvictedUnexpired` (action) | a live set entry is never changed | S "an unexpired entry MUST NOT be evicted" |
| `DeniedTokenStaysUsable` (action) | a step that sends a denied leaves the set unchanged | S "its token MUST stay usable for the retry" |
| `JunkTouchesNothing` (action) | delivering junk changes no server state | S _The Prefilter Header_ |
| `MappingCreatedOnce` (action) | a live mapping keeps its nonce | S "a mapping is created once" |

Checked in their own runs: `InWindowExact` (the window agrees with `opened` inside the
window; F1 breaks it), `DeniedMeansNoSlot` (a client at -1 has no slot; F5 breaks it),
`ExpiredNeverAdmits` (no slot was ever assigned under an expired token; the response
path breaks it, finding 7 below), `GatePassedOnlyAtSaturation` (see "The clock" below),
`BindingFirstAddress` (not run, see "What was not done").

Liveness (under `Spec`, i.e. `PeerFairness /\ PathSettles`; with the attacker off):
`HonestConnects` (state 1 leads to 4), `HonestResolves` (1 leads to 4 or -1),
`ServerNotStranded` (an established, unconfirmed slot is confirmed or goes away),
`HalfBounded` (a half-open mapping leaves; needs `Deadline > 0`, not run).

### Fairness, and why Link's is restated

`Link!Fairness` is not conjoined as written. Its `SF` ranges over every packet value,
and `ClientPackets` here is large enough (two addresses, four tokens, every cookie
value) that TLC refuses the temporal formula's disjunctive normal form. The module
restates it at the grain it needs:

- `PeerFairness`: `WF` on `Tick` and on every peer action, including every receive
  action. `Tick` is disabled while a copy at the head of either wire is due
  (`NothingDue`), so a due head is delivered before time moves on: latency is bounded
  by `MaxDelay` ticks, a tick is one send interval, and this is a stronger form of
  Link's `SF` on `Deliver`. It also gives the cookie lifetime its meaning.
- `PathSettles`: eventually no step is the path's own (`<>[][~Environment]`). TLC
  found, without it, the path duplicating one challenge forever: deliver a copy, make
  another, so the wire never empties and `Tick` never fires. A real path duplicates a
  given packet finitely often. The loss patterns are decided at `Send` and are not
  affected.
- `KindDelivery`: Link's `EventualDelivery` per packet kind instead of per packet value.
  `SpecArbitrary` is `PeerFairness /\ KindDelivery` for liveness under `"arbitrary"`,
  where `Drop` must stay enabled forever and `PathSettles` cannot be assumed; run it
  with `Duplication = FALSE` and `Reordering = FALSE`.
- `AttackerSettles`: eventually no step is the attacker's. TLC's first counterexample
  to `HonestConnects` with the attacker on was an attacker that injects a request every
  time the wire empties, so `Tick` never fires and the honest client never sends: an
  attacker of unbounded rate, the flood no protocol survives. Liveness with the
  attacker on is about what a bounded attacker leaves behind.

Safety never relies on any of these. Nothing is assumed of other clients.

### The clock

`clock` saturates at `MaxClock`. Once it has, nothing expires any more, so a liveness
property about the cookie gate passes vacuously after saturation. The gate is checked
as an invariant instead: `GatePassedOnlyAtSaturation` says the honest client never gets
past the gate while time is still moving. It is expected to **hold** for the broken
settings (a cookie lifetime of one send interval; a rotation every tick with no
previous-key acceptance) and to be **violated** for the working ones. Token expiry is
`ExpTime(t) <= clock`, absolute, so an expiry run sets `HonestExpire` below `MaxClock`.

## What the model abstracts away

- The signature, the seal, the MAC and the AEAD as computations: each is "the attacker
  cannot make one". The integrity limit (2^36) is unreachable at this size and is not
  counted.
- The salt (one attempt), the ephemeral pair and its erasure (F4 is the client
  connection module's), the server's S2C replay window (same code as C2S's; the client
  accepts any keep-alive under its keys), ack blocks and payloads, the client's
  deadlines and timeouts (the client never gives up except on a denied).
- The history's duplicate-entry corner: when a token's own entry has expired the code
  may insert a second entry for it in a free slot and find the new one first; the model
  overwrites the expired entry. Same visible behaviour, one slot less consumed.
- The handshake sequence numbers of challenge and denied, and the client's handshake
  replay window: handshake types are exempt from replay marking (`:716-735`).
- Ack blocks, and with them the code's keep-alive test (`keep_alive_acked`). The model
  stops the keep-alive at confirmation, which in the model only a client keep-alive
  sets (see "The server" above). The code's comment (`:1284-1294`) names the failure
  that substitution hides: a conformant client still in 3 may send a standalone ack,
  which would set the confirmed flag before the client has its index. The model's
  client sends nothing sealed but responses in 3, so that failure cannot occur in this
  model, and this model says nothing about it.
- The honest client leaving (disconnect, crash). F7's property
  `SlotReleasedAfterClientGone` therefore has no run here; the refresh it rests on
  (`ServerRecvResponse`, the established branch) is modelled and `ReplaySealed` reaches
  it.
- Other clients are a count (`others`) taking and releasing slots; they do no
  handshake and present no token, so F9's two-address trace is not reachable (see
  "What was not done").

## How to run it

On a Linux bench with the TLA+ tools (`tla2tools.jar`), from `tla/`:

```sh
java -XX:+UseParallelGC -Xmx12g -cp /path/to/tla2tools.jar tlc2.TLC -workers 16 -config MCSecurity.cfg MCSecurity
```

`MCSecurity.cfg` is the committed run: the attacker on, holding all four tokens
(`Tokens = {"th", "tx", "tf", "te"}`, `Signed = {"th", "tx", "te"}`,
`ForThisServer = {"th", "tf", "te"}`), so it presents the honest client's captured
token, a foreign one, a forged one and an expired one; both links losing their first
send (`Pattern = "first"`), `Capacity = 1`, no duplication or reordering,
`MaxClock = 2`, `MaxSeq = 1`, `MaxNonces = 1`; `SafetyInvariants` and the four action
properties. It finishes in under a minute on vision (1,233,227 states generated,
499,144 distinct, depth 30, on 8 workers).

That the three bad tokens are really presented is checked by reversed witnesses. Each
deletes one refusal from `ServerRecvRequest` in a scratch copy of `Security.tla` and
runs the committed config, plus probe properties from a scratch `MCSecurityProbe.tla`:
no mapping holds `"tx"`, no mapping holds `"tf"`, and no mapping is created for a token
already expired. Without the `ForThisServer` test the attacker maps `"tx"`, and without
the `Signed` test it maps `"tf"`: in both, `SafetyInvariants` (`NothingFromUnkeyed`)
fails in 7 states (inject, cookie, inject again with the cookie seen, mapped). Without
the expiry test it maps `"te"` after a tick, and the probe fails in 8 states. With
nothing deleted, all the probes and `ExpiredNeverAdmits` pass, with the same counts as
the committed run. The scratch files are not committed; `RUNS.tsv` records the four
runs.

The other runs in `RUNS.tsv` start from the **base instance**: the committed file with
`Capacity = 2`, `MaxClock = 4`, `MaxNonces = 2`, `MaxSeq = 3`,
both patterns `"arbitrary"`, `Duplication = TRUE`, `Reordering = TRUE`; each row says
what it changed from there. `Attacker = FALSE` for every liveness run. For example, the
F1 run from the base instance:

```sh
sed -e 's/ClearWords    = 2/ClearWords    = 1/' -e 's/MaxSeq        = 3/MaxSeq        = 8/' \
    -e 's/Attacker       = TRUE/Attacker       = FALSE/' \
    -e 's/S2CPattern    = "arbitrary"/S2CPattern    = "none"/' \
    -e 's/MaxClock      = 4/MaxClock      = 10/' -e 's/^    SafetyInvariants$/    SafetyInvariants\n    InWindowExact/' \
    base.cfg > /tmp/f1.cfg
java -cp /path/to/tla2tools.jar tlc2.TLC -workers 8 -config /tmp/f1.cfg MCSecurity
```

Common to every run: `Addrs = {"H", "A"}`, `MaxClients = 1`, `MapCap = 2`,
`HistoryCap = 2`, `AdmittedCap = 2`, `CookieLife = 2`, `Words = 2`, `WordBits = 2`
(W = 4), `MaxDelay = 0`; `Capacity = 2` everywhere but the committed run.

| Run | Changes to the base instance | Checks | Result |
|---|---|---|---|
| committed | `MCSecurity.cfg` as committed (above): four tokens, `Capacity = 1` | `SafetyInvariants`, the four action properties | passes |
| token witnesses | the committed config on a scratch copy with one refusal deleted (`ForThisServer`, `Signed`, expiry), plus the scratch probes | `+ ExpiredNeverAdmits` and the probes | each **fails** as expected; with nothing deleted, passes |
| attacker-on, larger | the base as is; and with fewer tokens, `"first"` or `"alternate"` or `"firstkind"`, no reordering, `MaxClock 3`, `MaxSeq 2`; and `HonestExpire = 2` | the same | **not finished** in the budget (11 to 47 million states each); no violation in the states explored |
| live-first / alternate / burst | `Attacker = FALSE`, the pattern on both links | `+ HonestConnects, ServerNotStranded` | passes |
| live-firstkind | `Attacker = FALSE`, `KindLoss = "firstkind"`, patterns `"none"` | the same | passes |
| live-arbitrary | `Attacker = FALSE`, `SPECIFICATION SpecArbitrary`, `Duplication`, `Reordering = FALSE` | `+ HonestConnects` | passes |
| hang-witness | live-firstkind with `RepeatKeepAlive = FALSE` | `+ HonestConnects, ServerNotStranded` | **fails**: the hang |
| hang-witness-alt | live-alternate with `RepeatKeepAlive = FALSE` | the same | **fails**: the hang |
| f1-code | `Attacker = FALSE`, `ClearWords = 1`, `MaxSeq = 8`, `MaxClock = 10`, S2C `"none"` | `+ InWindowExact` | **fails**: F1 |
| f1-fixed | f1-code with `ClearWords = 2` | the same | passes |
| f5-denied | `Attacker = FALSE`, `OthersMayJoin`, `OthersMayLeave = TRUE`, patterns `"none"` | `+ DeniedMeansNoSlot` | **fails**: F5 |
| expiry-admits | `Attacker = FALSE`, `HonestExpire = 2`, patterns `"none"`, no reordering, `MaxClock 3`, `MaxSeq 2` | `+ ExpiredNeverAdmits` | **fails**: finding 7 |
| cookie-race | `Attacker = FALSE`, `CookieLife = 1`, patterns `"none"`, no duplication or reordering | `+ GatePassedOnlyAtSaturation` | holds: the client never passes the gate |
| cookie-ok | cookie-race with `CookieLife = 2` | the same | **violated**: the gate is passed |
| rotation-witness | `Attacker = FALSE`, `Rotation = TRUE`, `RotatePeriod = 1`, `AcceptPrevious = FALSE`, patterns `"none"`, no duplication or reordering | the same | holds: never past the gate |
| rotation-ok | rotation-witness with `AcceptPrevious = TRUE` | the same | **violated**: the gate is passed |
| attacker-live | the attacker on, patterns `"none"`, no duplication or reordering, `MaxClock 3`, `MaxSeq 2` | `+ HonestConnects` | **fails** without `AttackerSettles` (finding 6); with it, not finished |

## Loss patterns TLC explored

Both links under `"first"` (the first send on each link lost): the committed run with
the attacker, and the liveness run with duplication and reordering; `"alternate"` (the
1st, 3rd, 5th send lost: "first packet dropped, second comes through") with duplication
and reordering; `"burst"` (sends 1 and 2 lost) likewise; `"arbitrary"` (any copy
dropped at any time) for the F1 runs and the arbitrary liveness run; and
`KindLoss = "firstkind"`, the first packet of every kind lost on both ends, with both
patterns `"none"`. The attacker-on runs under `"arbitrary"`, `"alternate"` and
`"firstkind"` with more than one token at `Capacity = 2` did not finish and are recorded
as such.

## What TLC found

Every counterexample below was replayed by hand through the functions it names. Two
are defects already in the inventory, reproduced as the inventory asked; one is the
inventory's question F5; the rest are model errors fixed, witnesses, or expected
behaviour of the design.

1. **F1, reproduced (defect, `sdk/rocketnet_replay.cpp:54-60`).** f1-code, 34 states.
   The client opens sequences 1, 2, 3 (`win = (0 :> {1} @@ 1 :> {0, 1})`, `hi = 3`);
   the link drops 4, 5 and 6; 7 arrives. `Advanced(3, win, 7)`: `skipped = 4 >= W`,
   so the first `ClearWords = 1` word is zeroed and bit `7 % 4 = 3` (word 1) is set,
   which was already set by sequence 3; `hi = 7`. Now for `s = 6`: `6 > 7 - 4`, so the
   bit decides, and bit `6 % 4 = 2` sits in word 1, still set from sequence 2.
   `AlreadyReceived(7, win, 6) = TRUE` while `6 \notin opened`: a genuine late packet
   is refused as already received. Through the code with its constants: a skip of 4096
   or more zeroes `window[0..3]` (bits 0..255) of 64 words, and every stale bit from
   256 up survives. Replayed against `rocketnet_replay_advance` and
   `rocketnet_replay_already_received` line by line; the inventory's F1 trace at full
   scale gives the same shape. f1-fixed (`ClearWords = Words`) passes the same run:
   clearing every word is the fix. Not changed on this card.

2. **F5, reproduced (the inventory's question, `sdk/rocketnet_server_protocol.cpp:514-576`).**
   f5-denied, 17 states. The client is at 3 and sends its response; the link
   duplicates it; another client takes the last slot (`OtherJoins`); the server reads
   the first copy, finds no slot, sends a denied and keeps the mapping; the other
   client leaves; the server reads the second copy, the slot is free, the set takes
   `"th"` and the slot is assigned (`admits["th"] = 1`, `admitted["th"] = 5`); the
   denied reaches the client, which is at 3, so it goes to -1. `DeniedMeansNoSlot` is
   violated: the server holds a slot, an established mapping and an admitted token
   for a client that has given up, until the idle timeout. Replayed through
   `rocketnet_server_process_connection_response` (the `num_free` check at `:514`, the
   denied at `:528-544` with the mapping left, the admit at `:546-576`) and
   `rocketnet_client_process_denied` (state 3, to -1). Whether this is a defect is the
   maintainer's call, as the inventory says; the token is consumed by a connection
   that will never confirm, and a retry with the same token is refused as already
   admitted for the token's lifetime.

3. **The hang class the request named (witness, not a defect in the code).**
   hang-witness: with `RepeatKeepAlive = FALSE` and the first packet of every kind
   lost, the server assigns the slot and sends the establishing keep-alive once; it is
   the first keep-alive, so it is lost; the client at 3 retransmits its response every
   tick; the server opens it, finds the mapping established, refreshes the last-received
   time and ignores it (`:778-783, 491-496`); nothing ever sends another keep-alive.
   `HonestConnects` and `ServerNotStranded` fail with a lasso of response, refresh,
   tick. hang-witness-alt is the same under `"alternate"`. With the code's behaviour
   (`RepeatKeepAlive = TRUE`: re-sent on the timer until a client keep-alive confirms
   the slot) the same runs pass (live-firstkind, live-alternate). The code
   (`rocketnet_server_protocol_write_keep_alive_if_due`,
   `sdk/rocketnet_server_protocol.cpp:1239-1316`) sends at least as many keep-alives as
   this: it re-sends on the keep-alive's own clock until the client acknowledges one,
   and keeps sending after that, so the model's re-send is a lower bound on the code's.
   The code already does what the request asks for this packet. The cookie, the challenge and the denied are not sent on a timer; they
   are re-provoked by the client's retransmitted request or response, which the client
   sends on its timer, and the four liveness runs show that suffices.

4. **The cookie race (expected; the standard's condition).** cookie-race:
   `CookieLife = 1`, one send interval. The cookie is issued at clock `c` with
   `exp = c + 1`; the client carries it in the request it sends after the next tick, at
   clock `c + 1`; the gate reads `exp > clock` as false, answers a fresh cookie, and the
   loop repeats until the clock saturates. `GatePassedOnlyAtSaturation` holds. With
   `CookieLife = 2` it is violated at clock 2: the gate is passed. This is STANDARD.md's
   "with lifetime at least two send intervals" made exact; the code's 3 s against a
   100 ms send interval is far inside it.

5. **The rotation window (F2's specification).** rotation-witness: the key rotates
   every tick and the previous key is not accepted, so a cookie issued under one
   generation is presented under the next and refused forever;
   `GatePassedOnlyAtSaturation` holds. rotation-ok, with `AcceptPrevious = TRUE`, is
   violated at clock 2. This is the model the rotation code (absent today, F2) is
   written against: rotate, keep the previous key for one period.

6. **The attacker of unbounded rate (model assumption, `AttackerSettles`).**
   attacker-live without `AttackerSettles`: `HonestConnects` fails with a lasso of
   `InjectRequest`, `ServerRecvRequest`, `InjectRequest`: the attacker puts a request
   on the wire every time it empties, `Tick` waits for the wire to drain (`NothingDue`),
   so time never passes and the honest client, which sends on the tick, never sends.
   That is an attacker faster than the link, the flood no protocol survives, and not a
   statement about the design; `AttackerSettles` excludes it from the liveness runs.
   With it, the bounded-attacker run (attacker-live, and the one-token
   attacker-live-small) did not finish inside the card's budget; what TLC would be
   looking for there is the trace in which the attacker, having seen the honest
   client's first request (the token travels in the clear), takes a cookie for its own
   address and presents the honest token from it before the honest client's
   cookie-carrying request arrives: the history binds the token to the attacker's
   address (`:283-288`), the attacker gets a challenge it cannot open, and the honest
   client is refused at the history check for the token's lifetime. Replayed by hand
   through `rocketnet_server_protocol_process_connection_request`, that trace is
   reachable in the model and in the code; the standard's _used-token history_ says a
   captured request "cannot be replayed from elsewhere", and the replay indeed admits
   nothing, but it also denies the token's owner, by an attacker who only needs to see
   one request and send from its own address. The bound is the token's expiry, which
   the standard says SHOULD be short. Recorded as an observation, not a defect, and
   not yet as a TLC-printed counterexample.

7. **A token that expires during the handshake still admits (observation,
   `sdk/rocketnet_server_protocol.cpp:478-576`).** expiry-admits, 12 states:
   `HonestExpire = 2`. The request passes the expire check at clock 1 and the challenge
   is sent; at clock 2 the token has expired; the response arrives at clock 2 and
   `rocketnet_server_process_connection_response` has no expire check, so the set
   takes `"th"` with an expire time already past (the entry reads as absent at once)
   and the slot is assigned. `ExpiredNeverAdmits` is violated. The standard's
   _Processing Connection Response Packets_ lists no expire check either, so the code
   matches the standard; the window is the handshake deadline (5 s). A retry with the
   token is refused at the request path, so nothing admits twice. For the maintainer:
   whether step 3 should re-check `mapping->token_expire_timestamp`.

8. **Model errors found by TLC and fixed.** (a) The first draft's keys were the nonce
   alone: TLC had the honest client accept a challenge the server had sealed under
   `"te"`'s keys for a mapping the attacker created at the honest address, and go on to
   connect under a token it never held. Keys are `[tok, nonce]` now, and that run
   showed the real behaviour instead: the attacker's spoofed request with a genuine
   token and the honest client's captured cookie occupies the honest address's mapping
   until the handshake deadline, and the honest client cannot open the challenge; the
   mapping is bounded by the deadline and nothing is admitted. (b) Link's per-packet
   `SF` made TLC refuse the fairness formula; restated per action and per kind, above.
   (c) Without `PathSettles` TLC duplicated a challenge forever; above.

## What was not done

- The committed attacker run holds all four tokens but only at `Capacity = 1`, so at
  most one packet is in flight on each link: the attacker's flood is one packet at a
  time, and an attacker packet never shares the wire with an honest one. No attacker
  run with more than one token finished at `Capacity = 2`: the eight base-instance
  runs (11 to 47 million states generated each, depth 14 to 19), and the committed
  config with `Capacity = 2` and `MaxNonces = 2`, which stopped at its 25-minute cap
  (27.7 million states generated, 12.3 million distinct, depth 19, 3.4 million left on
  the queue). None found a violation in the states explored. The choice of source,
  token and cookie the attacker has at every step is what grows the graph. `RUNS.tsv`
  records every unfinished run with the states it reached.
- The bounded-attacker liveness run (`AttackerSettles`) did not finish; finding 6
  describes the trace it would be looking for, replayed by hand and not yet printed by
  TLC.
- `BindingFirstAddress` (expected to fail by flush-then-replay once the history evicts
  at capacity) was not run: the honest client is the only key holder, so a second
  address that passes the history after an eviction can only be the attacker's, which
  the attacker-live run already shows admits nothing.
- F9's two-connections-under-one-token trace needs two addresses holding one token's
  keys (a shared token); this instance has one key holder, so `OneConnectionPerToken`
  holds here without settling F9.
- `HalfBounded` with `Deadline > 0` and F7's `SlotReleasedAfterClientGone` with a
  departing client and `Timeout > 0` are not run; the actions exist, the properties
  need a client that leaves.
- The server's S2C replay window and the keyed prefilter (not implemented by 1.0) are
  not modelled.
