# Security: tokens, the cookie gate, the tables, the keys, the replay window, with an attacker

**Full verification status: HOLD.** The shared-token witness admits an unexpired
credential twice, including with the configured table ratios and a finite handshake
deadline. The separate expiry invariant also fails. The committed small safety gate
passes only its stated scope; fairness-alone liveness and keyed-prefilter behavior
remain unproved. See [attempt 3](#attempt-3-shared-credentials-and-the-full-stop-2026-10-07)
for current evidence and source replay.

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
and its epoch window, MAC inputs and downgrade rejection are not modelled.
That is an explicit unfulfilled part of this card, not covered by `JunkTouchesNothing`, and
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
carries; the server holds its mapping's pair. With `KeyHolders = {}` the
attacker can replay a sealed packet and never make one. Additional `(address, token)`
pairs in `KeyHolders` may derive their own mapping's keys only after the challenge
is delivered on S2C; `HolderResponse` sends their sealed responses through C2S.
This models shared bearer credentials separately from the passive packet attacker. The salt is per attempt and
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
keep-alive is re-sent on the timer while the mapping is established, including after
slot confirmation. In `rocketnet_server_protocol_write_keep_alive_if_due`
(`sdk/rocketnet_server_protocol.cpp:1239-1316`), `keep_alive_acked` selects the timer:
`last_keep_alive_send_time` before acknowledgment, `last_send_time` after it.
`sdk/rocketnet_ack.cpp:385` latches that signal only when an ack block names a sent
keep-alive. Neither this signal nor slot confirmation stops keep-alives. This model
omits server payloads and standalone acks, so only keep-alives advance those send
clocks for an established mapping; `sdue` abstracts their common periodic send.
It does not verify ack-specific suppression under competing traffic.
`RepeatKeepAlive = FALSE` is a counterfactual witness that sends it once.

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
`ReplaySealed`, `HolderResponse`; `Tick` (the clock, both links age, the handshake deadline and the idle
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

These restrictions do **not** establish the card's unrestricted fairness-alone
liveness requirement. `PathSettles` requires all autonomous link activity eventually
to stop, which is stronger than finite duplication of each individual packet.
Per-kind `KindDelivery` is also not identical to per-value Link fairness. The checked
liveness claims are limited to the specifications and configurations listed here.

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

`MCSecurity.cfg` is the committed run: the attacker on, with all four tokens
(`Tokens = {"th", "tx", "tf", "te"}`, `Signed = {"th", "tx", "te"}`,
`ForThisServer = {"th", "tf", "te"}`), both links losing their first send
(`Pattern = "first"`), `Capacity = 1`, no duplication or reordering, `MaxClock = 2`,
`MaxSeq = 1`, `MaxNonces = 1`; `SafetyInvariants` and the four action properties.
This explores forged, foreign and already-expired token requests as well as replay
and junk flooding. Capacity 1 bounds the graph; it cannot exercise duplication or
reordering, which are checked in separate capacity-2 runs below. `ExpiredNeverAdmits`
is a separate negative check, not part of `SafetyInvariants`: a token that expires
between request and response can still admit (finding 7).

### Historical attempt-1 runs

The following table and original trace lengths describe snapshot
`9b731111b8bc185f71e020acc95131636fb102a5`, whose keep-alive abstraction stopped on
slot confirmation. They are retained as historical evidence, not fresh verification
of the corrected model. The fresh snapshot and results are listed separately below.

The historical runs in `RUNS.tsv` start from the **base instance**: the attempt-1 configuration with
`Tokens = {"th", "tx", "tf", "te"}`, `Signed = {"th", "tx", "te"}`,
`ForThisServer = {"th", "tf", "te"}`, `MaxClock = 4`, `MaxNonces = 2`, `MaxSeq = 3`,
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

Common to every historical run: `Addrs = {"H", "A"}`, `MaxClients = 1`, `MapCap = 2`,
`HistoryCap = 2`, `AdmittedCap = 2`, `CookieLife = 2`, `Words = 2`, `WordBits = 2`
(W = 4), `Capacity = 2`, `MaxDelay = 0`.

| Run | Changes to the base instance | Checks | Result |
|---|---|---|---|
| attempt-1 committed | one-token configuration at the historical snapshot, Capacity 2 | `SafetyInvariants`, the four action properties | passes |
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

In the historical snapshot, both links under `"first"` (the first send on each link lost): the then-committed run with
the attacker, and the liveness run with duplication and reordering; `"alternate"` (the
1st, 3rd, 5th send lost: "first packet dropped, second comes through") with duplication
and reordering; `"burst"` (sends 1 and 2 lost) likewise; `"arbitrary"` (any copy
dropped at any time) for the F1 runs and the arbitrary liveness run; and
`KindLoss = "firstkind"`, the first packet of every kind lost on both ends, with both
patterns `"none"`. The attacker-on runs under `"arbitrary"`, `"alternate"` and
`"firstkind"` with more than one token did not finish and are recorded as such.

## What TLC found

The attempt-1 counterexamples below were replayed by hand through the functions they name. Two
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
   (`RepeatKeepAlive = TRUE`: `rocketnet_server_protocol_write_keep_alive_if_due`
   re-sends while established, selecting its timer using `keep_alive_acked`, not
   `confirmed`) the repaired abstraction is checked in the fresh runs below.
   Historical live-firstkind and live-alternate also passed their narrower abstraction. The code already does what the request asks for
   this packet. The cookie, the challenge and the denied are not sent on a timer; they
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
   token is refused at the request path, which alone does not settle concurrent half-open mappings or sharing. For the maintainer:
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

## Historical limitations through attempt 2

The current attempt below adds a shared-key-holder witness and an unrestricted-clock
fairness diagnostic. The remaining limitations here continue to apply.

- Attempt 1's larger attacker-on safety runs did not finish within its budget:
  their unfinished state counts remain in `RUNS.tsv`, prefixed with their historical
  snapshot. The corrected four-token committed configuration uses Capacity 1 and
  completes; it replaces the former one-token TEST configuration. Larger capacities,
  ongoing arbitrary path activity and attacker-on liveness are not established by
  this small safety run.
- Ack blocks and competing established server traffic are omitted. Continuing
  keep-alives after slot confirmation fixes this model's incorrect stopping rule;
  it does not verify the real dual-clock suppression behavior or malicious ack claims.
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


## Historical corrected-model verification (2026-10-06)

Source base: `c4f9a8c16a4b9f147a1536a2af4139d8c3b1eb29`; prior work carried from
`9b731111b8bc185f71e020acc95131636fb102a5`. The corrected model continues sending
keep-alives after slot confirmation. All fresh runs use TLC 2.19 (08 August 2024,
revision `5a47802`), Java 21.0.12.1, Linux amd64 on `space`, serialized with eight
workers. The source snapshot is identified by these SHA-256 values (documentation
and RUNS.tsv are result records, not TLC inputs):

| Input | SHA-256 |
|---|---|
| Security.tla | `b5736d13541f7dc5f0f7196bcd2b04f33ef30876097ed6b21525e94299ee4f5a` |
| MCSecurity.tla | `1db96ad5036b9aad03c33091a370c48b1612ac9ce74dbea583a4fb0c618a4bb9` |
| MCSecurity.cfg | `c90afae6d7ba95458d8dcb1459a5dadc47b318944273cd61c98719f0caa9f431` |
| Link.tla (unchanged) | `8371800d24fba8f7bbd63e3bfdea322a27ab3cc6f742cfa9d1be3c2b9dd1f669` |

Command, from `tla/`, using a separate state directory for each run:

```sh
java -XX:+UseParallelGC -Xmx8g -cp /path/to/tla2tools.jar tlc2.TLC \
  -workers 8 -metadir /path/to/run-states -config /path/to/run.cfg MCSecurity
```

The `committed` run uses MCSecurity.cfg unchanged. Every other **fresh** run starts
from that same committed file and applies the following common changes:
`Attacker = FALSE`, `Capacity = 2`, `MaxClock = 4`, `MaxSeq = 3`, `MaxNonces = 2`,
`Duplication = TRUE`, `Reordering = TRUE`. Keep all its original invariant and action
property checks; apply each row's additional changes and checks. In this section
“patterns” always means both C2SPattern and S2CPattern.

| Fresh run | Further changes | Additional checks |
|---|---|---|
| live-firstkind | KindLoss "firstkind", patterns "none" | HonestConnects, ServerNotStranded |
| live-first | patterns "first" | HonestConnects, ServerNotStranded |
| live-alternate | patterns "alternate" | HonestConnects, ServerNotStranded |
| live-burst | patterns "burst" | HonestConnects, ServerNotStranded |
| hang-firstkind | live-firstkind with RepeatKeepAlive FALSE | HonestConnects, ServerNotStranded |
| hang-alternate | live-alternate with RepeatKeepAlive FALSE | HonestConnects, ServerNotStranded |
| live-arbitrary | SPECIFICATION SpecArbitrary, patterns "arbitrary", Duplication FALSE, Reordering FALSE | HonestConnects |
| expiry | HonestExpire 2, patterns "none", Reordering FALSE, MaxClock 3, MaxSeq 2 | ExpiredNeverAdmits |
| denied | OthersMayJoin TRUE, OthersMayLeave TRUE, patterns "none" | DeniedMeansNoSlot |
| replay-code | ClearWords 1, MaxSeq 8, MaxClock 10, C2SPattern "arbitrary", S2CPattern "none" | InWindowExact |
| replay-fullclear | replay-code with ClearWords 2 | InWindowExact |

### Fresh trace replay against the current C++

- **hang-firstkind:** 29 states, lasso back to state 24. Admission sends the first
  keep-alive, lost by the kind-loss pattern; the client remains at 3 while its
  responses refresh the established mapping. **hang-alternate:** 34 states, lasso
  back to state 25, with the establishing keep-alive lost at its odd send position.
  In both, the response refresh matches server_protocol.cpp:778-783 and the connected
  early return at :491-496. The absent retry is deliberately injected by
  RepeatKeepAlive FALSE; actual :1239-1316 supplies the retry. These are counterfactual
  witnesses, not current-code hangs. The corresponding TRUE configurations pass.
- **expiry:** 12 states. At state 8 the request passes at clock 1, at state 10 the
  clock reaches token expiry 2, and state 12 admits. The response function at
  server_protocol.cpp:478-576 never checks token expiry; token_tables.cpp:269-290
  inserts the expired timestamp when a free entry exists. The token then reads as
  absent to later lookups. This confirms finding 7's observation, not a claimed
  violation of the standard's response ladder.
- **denied:** 17 states. Another client takes the slot at state 10; the response is
  duplicated at state 13; its first copy receives a denied at state 14; the client
  reaches -1 at state 15; the other client leaves at state 16; the remaining response
  admits at state 17. server_protocol.cpp:514-544 leaves the mapping after a denied;
  :546-576 later assigns the freed slot, while client_protocol.cpp:612-614 already
  moved the client to denied. This is the existing F5 resource-retention question.
- **replay-code:** 34 states. Sequences 1 and 2 open (states 16 and 19); 3 through 6
  are lost; 7 opens at state 34. Now `hi = 7`, `opened = {1, 2, 7}` and
  `win = (0 :> {} @@ 1 :> {0, 1})`. Sequence 6 is inside the window but its bit,
  inherited from sequence 2, says received. Replaying replay.cpp:54-60 shows exactly
  the defect: a jump of at least W clears only the first four of the real 64 words.
  The reduced model clears one of two words. replay.cpp:27-39 then trusts the stale
  in-window bit. This reproduces F1; no C++ was changed.

Fresh counts and outcomes follow. Error-state counts depend on worker scheduling;
the recorded trace and full configuration identify the checked behavior.

| Run | States generated | Distinct | Outcome |
|---|---:|---:|---|
| committed | 1291095 | 516800 | pass |
| live-firstkind | 17644 | 8331 | pass |
| hang-firstkind | 6188 | 3403 | expected counterfactual hang |
| live-first | 17016 | 7491 | pass |
| live-alternate | 5623 | 2717 | pass |
| hang-alternate | 2311 | 1245 | expected counterfactual hang |
| live-burst | 11130 | 4890 | pass |
| live-arbitrary | 25530 | 12008 | pass |
| expiry | 278 | 211 | expected violation: ExpiredNeverAdmits (response expiry observation) |
| denied | 4384 | 2195 | expected violation: DeniedMeansNoSlot (F5 question) |
| replay-code | 720295 | 278978 | expected violation: InWindowExact (F1 defect) |
| replay-fullclear | 6473605 | 2225188 | pass |

The committed gate printed:

```text
Model checking completed. No error has been found.
1291095 states generated, 516800 distinct states found, 0 states left on queue.
Finished in 16s at (2026-10-06 19:18:55)
```

No new C++ defect is claimed by the keep-alive correction. The new safety TEST covers
all four tokens, but does not erase the separately observed response-expiry behavior,
the single-key-holder limitation, or the liveness fairness restrictions above.

## Attempt 3: shared credentials and the full STOP (2026-10-07)

This attempt starts at current `origin/main`,
`c4f9a8c16a4b9f147a1536a2af4139d8c3b1eb29`. It carries attempt 1 and the two
corrections from `53ce98a52a0a1b3279121ffeda7cabbf15c77d9d` (all four attacker
tokens and keep-alives continuing after confirmation). The historical runs above
remain evidence for their recorded snapshots, not for every new setting here.
The committed configuration now uses `ClearWords = 1`, representing the code's
partial-clear behavior; the full-clear setting is only a comparison run.

`KeyHolders` adds explicit shared-token possession. A holder learns a key only
when `OtherRecv` delivers its own challenge on S2C. `HolderResponse` sends its
response on C2S. This does not grant the passive replay attacker token secrets or
another holder's ephemeral exchange. Empty `KeyHolders` preserves the old baseline.
`MCSharingSpec` restricts ordinary model actions to one admission-race scenario;
it adds no synthetic state transition. It is a witness search, not exhaustive
verification of every multiple-holder behavior.

### New counterexample: one unexpired token admits twice

The reduced witness has one slot, two mapping entries, history capacity one,
three addresses, and two genuine tokens. At state 8 the honest address has a
half-open mapping for `th`; another client occupies the only slot at state 9.
At state 15 a genuine `tb` request records its history entry, evicting `th`, and
is denied for lack of slots. The other client leaves (state 16). At state 22,
address A presents shared token `th` and creates a second half-open mapping.
A receives its challenge at state 23. The honest response admits at state 26;
the honest connection then times out at state 28. A's response admits at state
30: `admits["th"] = 2`, `admitted["th"] = 5`, and `clock = 3`. The token is
unexpired throughout. `TokenAdmitsAtMostOnce` fails inside `SafetyInvariants`;
`OneConnectionPerToken` still holds because these connections are sequential.

Hand replay against the current source:

1. `sdk/rocketnet_server_protocol.cpp:283-288` checks the history;
   `:352-357` checks the admitted set only on the request path; `:359` records
   history before the free-slot test at `:395-413`. Thus genuinely issued requests
   denied for a full server still evict history entries, without occupying mappings.
2. `sdk/rocketnet_token_tables.cpp:160-187,233-248` evicts the oldest history entry.
   After eviction and release of the other client's slot, a second request for `th`
   passes `contains` because neither `th` mapping has admitted yet. Both mappings
   are inside the two-entry mapping cap, with separate ephemeral exchanges.
3. The first response consumes `th` at `server_protocol.cpp:521` and assigns a slot
   at `:546-550`. Its idle timeout releases that slot and mapping at `:1049-1055`.
   The pending second mapping and the admitted entry survive.
4. The second response passes the already-connected-address and client-id checks
   at `:491-503`. `rocketnet_admitted_token_set_add` returns **success** for the
   existing unexpired entry at `token_tables.cpp:277-278`. There is no response-time
   `contains` rejection, so `server_protocol.cpp:546-550` admits again.

This is a current-code defect against the standard's “a token admits one
connection” promise, not an expired-token or simultaneous-slot claim. No C++ is
changed on this card. A larger history requires more genuine filler tokens; it
does not turn the request-only admitted check into a response-time consume-once
check. The finite-deadline and eight-entry-history witness runs below check those
specific boundaries rather than relying solely on this scaling argument.

### Full acceptance remains blocked

- `ExpiredNeverAdmits` still fails: request at clock 1, expiry at 2, response
  admission at 2. `server_protocol.cpp:478-576` has no response-time expiry check;
  `token_tables.cpp:269-290` accepts an already-expired timestamp in a free entry.
  The response ladder in the standard omits that check too. This is a mismatch
  between this card's invariant and the standard/code, and needs a design decision.
- Passing first-kind and alternate-loss liveness retains `PacedTicks = TRUE`,
  `PathSettles`, no attacker, no client deadlines, and a saturated clock. The new
  `SpecFair` diagnostic removes `PathSettles` and `PacedTicks` but still has
  per-kind delivery fairness, not the requested per-packet fairness. It cannot
  establish the full fairness-alone statement, regardless of its outcome.
- Keyed prefilter epoch rotation, live-generation membership, MAC field binding
  and rejection of epoch-zero downgrade are not modelled. STANDARD.md:417-421
  explicitly says this keyed form is specified but unimplemented in 1.0;
  the cookie-key rotation in this model is a different mechanism.
- Ack blocks, payload competition, S2C replay, key erasure, and the backend
  connected set are abstracted away. Existing code retries establishing
  keep-alives at `server_protocol.cpp:1239-1316`, but this security model cannot
  prove the real ack-dependent suppression rule with competing traffic.
- `MaxSeq`, `MaxNonces`, clock saturation and link capacity are finite model
  bounds. No passing bounded run proves the unbounded protocol or all deployments.

### Reproduce the attempt-3 runs

Use TLC 2.19 (08 August 2024, revision `5a47802`), Java 21.0.12.1,
Linux amd64. Runs use two workers, 4 GiB heap, a 300-second per-run limit,
and a unique state directory (the final canonical gate has a 120-second limit). The bench is `hetzner`; its shared TLC lock is
`/tmp/stella-tlc-bench.lock`. No TLC or Go gate ran on the workstation.
Queued runs on `vision` were cancelled before Java started, to leave its busy
bench available to another card.

From `tla/`, with each configuration generated by applying the listed overrides
to the committed `MCSecurity.cfg`:

```sh
flock /tmp/stella-tlc-bench.lock timeout 300s \
  java -XX:+UseParallelGC -Xmx4g -cp /opt/tla/tla2tools.jar tlc2.TLC \
  -workers 2 -lncheck final -metadir states-RUN -config RUN.cfg MCSecurity
```

The table changes constants unless it says SPECIFICATION or adds a check. Keep
all the committed invariants and action properties. “patterns” means both link
patterns. `live` overrides are: `Attacker FALSE`, `Capacity 2`, `MaxClock 4`,
`MaxSeq 3`, `MaxNonces 2`, `Duplication TRUE`, `Reordering TRUE`,
`ClearWords 2`, patterns `"none"`.

| Run | Overrides to the committed configuration | Added checks |
|---|---|---|
| final-gate | none; direct MCSecurity.cfg, hashes recorded before TLC | none |
| committed-code | none | none |
| committed | ClearWords 2 (initial carry check) | none |
| expiry | Attacker FALSE, HonestExpire 2, MaxClock 3, patterns "none", ClearWords 2 | INVARIANT ExpiredNeverAdmits |
| live-firstkind | live; KindLoss "firstkind" | PROPERTIES HonestConnects ServerNotStranded |
| live-alternate | live; patterns "alternate" | PROPERTIES HonestConnects ServerNotStranded |
| hang-firstkind | live; KindLoss "firstkind", RepeatKeepAlive FALSE | PROPERTY HonestConnects |
| fairness-only | live; SPECIFICATION SpecFair, PacedTicks FALSE, patterns "arbitrary" | PROPERTY HonestConnects |
| replay-code | Attacker FALSE, Capacity 2, MaxClock 10, MaxSeq 8, MaxNonces 2, Duplication TRUE, Reordering TRUE, C2SPattern "arbitrary", S2CPattern "none" | INVARIANT InWindowExact |
| replay-fullclear | replay-code; ClearWords 2 | INVARIANT InWindowExact |
| safety-arbitrary | Attacker FALSE, Capacity 2, MaxClock 2, MaxSeq 2, MaxNonces 1, Duplication TRUE, Reordering TRUE, MaxDelay 1, patterns "arbitrary" | none |

The `sharing` witness uses SPECIFICATION `MCSharingSpec`, `Addrs {"H","A","B"}`,
`Tokens`, `Signed` and `ForThisServer` all `{"th","tb"}`, `KeyHolders <- MCSharingHolders`,
`MaxClock 4`, `HistoryCap 1`, `MaxNonces 3`, `Timeout 1`, `OthersMayJoin TRUE`,
`OthersMayLeave TRUE`, patterns `"none"`, and `ClearWords 2`. The other constants
are unchanged. `sharing-bounded` additionally sets `Deadline 5`. `sharing-cap8`
starts from `sharing-bounded`, sets `HistoryCap 8`, `MaxNonces 10`, and changes
all three token sets to `{"th","tb1","tb2","tb3","tb4","tb5","tb6","tb7","tb8"}`.
`MCCid` assigns the filler tokens client id 5; they are denied, never connected.
`MCExpire` makes each filler token unexpired. The `CHOOSE` in `MCSharingNext`
selects the next not-yet-recorded filler token, reducing irrelevant permutations.
This is an explicit scenario restriction, not a claimed symmetry proof.

### Counterexample validation and limits

`sharing-bounded` reproduces the 30-state violation with `Deadline = 5`.
`sharing-cap8` reproduces it in 51 states with the actual table ratios for one
configured slot: history 8, admitted set 2, encryption mappings 2. The honest
mapping is created at clock 1; eight genuine filler requests are denied while
another connection occupies the slot. The second `th` mapping is created at
clock 2. The first admission is state 47 at clock 2; timeout is state 49 at
clock 3; the second admission is state 51 at clock 3. The token's modeled expiry
is 5 and `lateAdmit` remains FALSE. The first mapping's handshake deadline is 6,
and the second's is 7, so disabling deadlines is unnecessary to this trace.

This is a **bounded TLC counterexample, replayed by hand through source**. It is
not an executable C++ integration-test reproduction. The source replay accounts
for the abstraction of `OtherJoins`/`OtherLeaves`: a real unrelated client uses
its own genuine token, history entry, admitted entry and mapping. During its stay
there are exactly two mappings (it and the honest half-open mapping); afterward
there are exactly two (the two `th` mappings). Its history entry does not prevent
the eight subsequent distinct genuine tokens from evicting `th`. Its admitted
entry plus `th` occupies the two-entry admitted set, and adding the already-present
`th` still returns success. No filler request creates a mapping because the
no-slots check runs first. The unrelated client's id differs from ids 1 and 5.

`Expire = 0` in the instance means beyond the modeled horizon, and is represented
by timestamp 5 here; it is not a claim that a real token with wire expiry zero
would be accepted. Concrete source replay uses genuine backend-signed tokens
with matching sealed fields, a future unix timestamp corresponding to model 5,
and a valid positive one-second idle timeout. Cryptographic primitives are ideal
and the two holders legitimately possess `th`; no signature forgery, replay-window
bypass or guessed session key is used.

The new replay-window run fails at state 34: sequences `{1,2,3,7}` opened,
`hi = 7`, and the second word still has bits `{0,1}`. The 3-to-7 skip equals W=4.
`replay.cpp:54-60` clears only the first modeled word, so sequence 6 falsely
reads as received although absent from `opened`. This is the same F1 defect
verified previously; `replay.cpp:27-39` trusts the stale in-window bit. The model
comparison clearing every word does not change C++.

The new `hang-firstkind` run has 28 states and loops to state 23. The establishing
keep-alive is lost, `cstate` stays 3, and responses continue to refresh the
established mapping. Hand replay through `server_protocol.cpp:712-783,491-496`
confirms that response packets are deliberately exempt from replay marking and
are ignored after refreshing the established mapping. The missing keep-alive
retry is the injected `RepeatKeepAlive FALSE` behavior; current code supplies it
at `:1239-1316`. This counterexample validates the witness, not a current-code hang.

The `fairness-only` diagnostic reached its 300-second timeout before exhaustive
completion. Its last printed progress was 822,717 generated states, 186,821
distinct states, and 58,468 queued states. There was no printed counterexample;
this is an unfinished run, not a pass or proof of no hang.

The cheap committed gate has `MaxNonces = 1` and `MaxSeq = 1`: it cannot exercise
two mappings for one token or a jump across the four-bit replay window. Those
claims are tested separately, and the new sharing and partial-clear witnesses
fail. Replay-refusal invariants cover session packets; handshake responses are
intentionally exempt at `server_protocol.cpp:712-714,730,758-783`, so this model
also does not claim that every replayed packet of every kind is refused.
A concrete numerical hand replay of F1 at the real window size is: open sequences
256 and 257, lose 258 through 4352, then open 4353. The 4096-step advance clears
words 0 through 3 only, retaining bits 0 and 1 of word 4. Sequence 4352 is inside
the window and maps to word 4 bit 0, so `already_received` incorrectly rejects it
although it never opened. This is source-level arithmetic, not a newly run C++ test.

### Attempt-3 results

| Run | Generated | Distinct | Result |
|---|---:|---:|---|
| expiry | 67 | 52 | counterexample: ExpiredNeverAdmits |
| sharing | 437 | 359 | counterexample: SafetyInvariants (TokenAdmitsAtMostOnce; source hand replay, not executable reproduction) |
| committed | 1,291,095 | 516,800 | pass |
| live-firstkind | 17,644 | 8,331 | pass |
| live-alternate | 5,623 | 2,717 | pass |
| hang-firstkind | 6,188 | 3,403 | expected counterfactual: HonestConnects violated with retry disabled |
| fairness-only | 822,717 | 186,821 | unfinished at 300s limit; last printed progress only; no pass claimed |
| committed-code | 1,291,095 | 516,800 | pass |
| sharing-bounded | 433 | 355 | counterexample: SafetyInvariants (TokenAdmitsAtMostOnce; source hand replay, not executable reproduction) |
| replay-code | 855,043 | 328,334 | counterexample: InWindowExact |
| sharing-cap8 | 885 | 695 | counterexample: SafetyInvariants (TokenAdmitsAtMostOnce; source hand replay, not executable reproduction) |
| replay-fullclear | 3,011,417 | 1,110,888 | unfinished at 300s limit; last printed progress only; no pass claimed |
| safety-arbitrary | 28,234,594 | 3,157,648 | unfinished at 300s limit; last printed progress only; no pass claimed |
| final-gate | 1,291,095 | 516,800 | pass |

The final canonical gate records the following input hashes before TLC starts:

```text
cd286efeb87a3d92dc443a5386a908175b266718149a3dfbddae5bd01c4ded50  Security.tla
8c58d6d2489cd6b65abd7c26494961e5275b69d0530265447aa885eaa459f118  MCSecurity.tla
5f5eb1aab1ea8aa8eda7a2e3e34f731a30dcfb81ccca295a0c8233b36c7841cb  MCSecurity.cfg
8371800d24fba8f7bbd63e3bfdea322a27ab3cc6f742cfa9d1be3c2b9dd1f669  Link.tla
```

Its final output is:

```text
Model checking completed. No error has been found.
1291095 states generated, 516800 distinct states found, 0 states left on queue.
Finished in 35s at (2026-10-07 18:46:50)
```

The tool timestamp is the bench's local time (UTC+02); the run date is 2026-10-07.
The passing baseline does not override the failing witnesses or unfinished runs.
