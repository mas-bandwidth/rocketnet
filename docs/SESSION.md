# The Session Layer: Identity, Rolling Magic, and the Keyed Filter Value

This document is the design for RocketNet's optional session layer: identity
authentication with a backend (one identity, one seat, the old seat kicked),
the rolling magic that feeds the keyed filter value at epochs 1-255, and replay
protection in the kernel. It is laid out bottom up, in the order the layers
are built, each layer secured before the one above it starts. Every statement
of current behavior cites `file:line` at commit `9bca948` on
`sprint/rocketnet-proof`, plus the keyed filter value at `b3b567d`, the head of
the in-flight card `rocketnet-keyed-filter-value`.

Read it beside `STANDARD.md` (_The Prefilter Header_, _The keyed filter value_,
_Where the keyed form comes from, and what it is for_, _Connect token sharing,
and the connected set_, _Replay Protection_, _Check placement_) and
`docs/TLA-INVENTORY.md` (M3 the client slots, M5 the cookie key rotation, M7 the
replay window).

**Status: design for review. Nothing here is implemented beyond THE STATE, and
every wire or standard change in THE WIRE waits for the owner's approval.**

The layer is optional in the strict sense: a deployment with no backend runs
epoch 0 exactly as today and pays nothing. A deployment that turns the session
layer on gets all of it, because the keyed filter and the identity posture rest
on the same backend and arrive together or not at all (STANDARD.md:592-594).
It is also unavailable to player-hosted fleets, since a server that holds a
live magic is a trusted server (STANDARD.md:596-602).

The design is an extraction, not an invention. Where the title's own backend
already performs a step, this document says so as a question in QUESTIONS for
the owner to confirm rather than describing that backend.

---

## 1. THE STATE

### 1.1 The prefilter header and the epoch byte

- The header is 6 bytes on every packet: type, epoch, 4-byte value
  (`sdk/rocketnet_wire.h:34-39`, STANDARD.md:399-427).
- Epoch 0 is the only meaning implemented: `ROCKETNET_FILTER_EPOCH_UNKEYED`
  (`sdk/rocketnet_wire.h:41-46`). The test title's constant is `b7 59 01 00`
  (`sdk/rocketnet_wire.h:52`, STANDARD.md:485-486). No magic exists anywhere in
  the tree, by rule (STANDARD.md:548-557).
- Every sender writes epoch 0: `rocketnet_write_prefilter_header`
  (`sdk/rocketnet_protocol.cpp:20-25`).
- The userspace receive check refuses any epoch but 0 on a comparison, then
  compares the 4 bytes (`sdk/rocketnet_protocol.cpp:50-70`), with refusal codes
  `ROCKETNET_PREFILTER_WRONG_EPOCH` and `_WRONG_VALUE`
  (`sdk/rocketnet_protocol.h:35-40`) mapped to their counters
  (`sdk/rocketnet_protocol.cpp:30-48`, `sdk/rocketnet_counters.h:85-88`).
- The value is configured per transport: `filter_value` in the client and
  server config tables (`sdk/rocketnet.h:608`, `sdk/rocketnet.h:843`), copied in
  at create (`sdk/rocketnet_client_transport.cpp:286`,
  `sdk/rocketnet_server_transport.cpp:815`).
- The kernel filter holds one epoch and one value in its config map
  (`sdk/rocketnet_wire.h:138-146`, `filter_epoch` at :143, "0 in 1.0") and
  checks both before the type and size rule
  (`server_xdp/server_xdp.c:136-158`). Its decisions are judged as host C in
  `test_unit_xdp_filter_host.cpp` (tier 1) and by `BPF_PROG_TEST_RUN` in
  `test_xdp_filter.cpp` (tier 2), from the same rows.

### 1.2 The pure keyed-value pieces in flight

- `rocketnet-keyed-filter-value` (merging, head `b3b567d`) adds
  `rocketnet_keyed_filter_value( magic, packet_type, packet_length, four_bytes, out )`: SipHash-2-4 over type, little-endian length and the four bytes after
  the header, low 4 bytes out (`sdk/rocketnet_protocol.cpp:30-59` and
  `sdk/rocketnet_protocol.h:25-35` at `b3b567d`), with
  `test_unit_keyed_filter_value.cpp`. It is internal and wired into nothing.
- `rocketnet-tla-filter-epochs` (ready) models the receiver's live-generation
  table as `tla/RNFilterEpochs.tla`: `Install(g)`, `Retire`, `Check(e)`, with
  invariants `NeverKeyedAtZero`, `AtLeastTwoLiveAfterRotation`,
  `WrapSkipsZero` and `DeadEpochRefusedWithoutMac`.
- `rocketnet-filter-epoch-window` (waiting on both) adds
  `struct rocketnet_filter_epochs_t` (up to 4 live generations),
  `rocketnet_filter_epochs_install` and `rocketnet_filter_epochs_check` to
  `sdk/rocketnet_protocol.{h,cpp}`, each citing its RNFilterEpochs action, the
  retired magic wiped. Not wired into a receive path: that waits for this
  design.

This design builds on those three and adds no second path: every keyed check
below calls `rocketnet_filter_epochs_check`, and every keyed write calls
`rocketnet_keyed_filter_value`.

### 1.3 Identity: what the server already has

- The client id is inside the sealed private block, with 256 bytes of user data
  (`sdk/rocketnet_connect_token.h:46-54`, STANDARD.md:815-845).
- One server already enforces one seat per client id locally: a request or a
  response naming a connected client id is refused and counted
  (`sdk/rocketnet_server_protocol.cpp:525-528` and `:688-691`, counters
  `CONNECTION_REQUEST_CLIENT_ID_ALREADY_CONNECTED` and
  `CONNECTION_RESPONSE_CLIENT_ID_ALREADY_CONNECTED`), the rule of STANDARD.md:1002-1006, which `docs/TLA-INVENTORY.md` M3 names `OneSlotPerClientId`.
- The kick verb exists: `rocketnet_server_transport_disconnect_client`
  (`sdk/rocketnet.h:1074-1083`, exported at `tools/exports.txt:117`,
  implemented at `sdk/rocketnet_server_transport.cpp:1897`), and the message
  layer reaches it (`sdk/rocketnet_message_server.cpp:1135-1137`).
- The client id of a slot is public: `rocketnet_server_transport_client_id`
  (`sdk/rocketnet.h:1092`, `tools/exports.txt:110`).
- The user data is held per slot (`rocketnet_client_slots_assign` at
  `sdk/rocketnet_server_protocol.cpp:736`, accessor
  `rocketnet_client_slots_user_data` at `sdk/rocketnet_client_slots.h:61`) but
  **no public function returns it**, and none returns the token identifier
  (`rocketnet_connect_token_identifier`, a BLAKE2b of the signed connect data,
  `sdk/rocketnet_connect_token.cpp:221-226`) that names the session to a backend.
- There is no connect or disconnect event surface. A game learns of a new
  client by polling `rocketnet_server_transport_is_client_connected`.

### 1.4 The token's slack

The token is 768 bytes: 576 signed, 64 signature, 32 + 32 clear session keys,
and 64 bytes of zero pad outside the signature (STANDARD.md:897-916, written at
`sdk/rocketnet_connect_token.cpp:144`). STANDARD.md:579-583 names that pad as
the one place in the token a client-side secret could ride. The signed buffer
keeps 55 bytes free (STANDARD.md:889-892), but it travels in the clear in every
request, so nothing secret may go there.

### 1.5 Replay protection today

- One window per key phase per direction, a `highest_opened` and 4096 bits
  (`sdk/rocketnet_encryption_map.h:31-35`, `:59-60`;
  `ROCKETNET_HISTORY_PACKETS` at `sdk/rocketnet_history.h:8`).
- Check and advance are separate, and advance runs only after the open
  (`sdk/rocketnet_replay.cpp:29-71`), modelled by `tla/ReplayWindow.tla`
  (`WindowIsTruth` at :202, `NoAdvanceBeforeConnect` at :231).
- The standard's sizing rule is now "at least 4096 sequences"
  (STANDARD.md:1608-1632). TODO.md's session paragraph still quotes the older
  "never smaller than 256" rule; the tension it names is the same at either
  number and is resolved in 2.6.
- Nothing about replay runs in the kernel.

---

## 2. THE LAYERS

Bottom up. Each layer names what it owns, what it guarantees, and how it is
checked. A layer is secured (model with reversed witnesses, unit tests on every
refusal, a cold reader's probes) before anything above it starts.

### 2.1 L0: the keyed value and the receiver's generation table

**Owns.** `rocketnet_keyed_filter_value` and `rocketnet_filter_epochs_t`: up to
4 live `(epoch, magic)` pairs, newest last.

**Guarantees.** Epoch 0 is never keyed. A dead epoch is refused on a comparison
with no MAC computed. At least two generations are live after the first
rotation. Generations advance by one and wrap 255 to 1. A retired magic is wiped.

**Checked by.** `tla/RNFilterEpochs.tla` and the two code cards in 1.2. This
layer is in flight and is not redesigned here.

### 2.2 L1: how a generation reaches a server and a client

The client never ships a magic (STANDARD.md:560-563). It receives one at run
time, from two places, and the server receives its own on a third.

**To a server: the generation blob.** The backend already signs connect data
with Ed25519 and seals private data to a server's X25519 public key. A
generation reuses exactly those two primitives, so no new key and no
backend-to-server shared secret is introduced:

    signed generation data (64 bytes):
        [version] (uint32)              // an opaque constant, distinct from the token's
        [protocol id] (uint64)
        [epoch] (uint8)                 // 1-255
        <zero pad to 16>
        [not before] (uint64)           // unix time the generation may be sent at
        [expire timestamp] (uint64)     // unix time after which it is retired
        [server public key] (32 bytes)  // the X25519 key the magic is sealed to
    [sealed magic] (64 bytes)           // crypto_box_seal( magic, 16 bytes ): 16 + crypto_box_SEALBYTES (48)
    [signature] (64 bytes)              // over signed data and sealed magic

The server verifies the signature against `backend_public_key`, checks the
protocol id and its own public key, then opens the seal; verify before open,
exactly as for a token (STANDARD.md:3536-3538). How the blob travels (HTTP,
the title's own channel) is the title's business: the library takes bytes.

**To a client at connect: the token tail.** The 64-byte pad outside the
signature carries the generations the client needs to connect:

    [filter epoch] (uint8)              // 0 = no keyed generation, the 1.0 meaning
    [filter magic] (16 bytes)
    [next epoch] (uint8)                // 0 = none
    [next magic] (16 bytes)
    <zero pad to 64>

It is as confidential as the clear session keys beside it, because the same
secure channel delivers both, and a tampered tail fails the connect and nothing
worse (STANDARD.md:915-916). A token whose tail epoch is 0 is a 1.0 token.

**To a client in session: the filter packet.** A session outlives a
generation, so the server delivers each new generation inside the session, as a
new sealed session type sent server to client only:

    connection filter packet (type 10), sealed under the session keys:
        [ack block]
        [epoch] (uint8)
        [magic] (16 bytes)

The server sends it each send interval until it observes the client's packets
at that epoch. That observation is the acknowledgment, so there is no separate
ack and nothing new to remember: the client switching epoch proves it holds the
magic. Because types 10-255 are reserved and dropped today
(`sdk/rocketnet_wire.h:32`), and the server-inbound rule
(`sdk/rocketnet_wire.h:113-125`) admits only what a server receives, type 10
changes no server-side rule and is refused by every 1.0 client, which is the
correct behavior for a client that cannot hold a keyed generation.

**Owns.** The three formats, their writers and readers, and their size and
content checks (padding required on read, STANDARD.md:3594-3597).

**Guarantees.** A server holds a magic only from a blob the backend signed for
that server's key. A client holds only magics delivered over the secure channel
or under the session keys. No magic is ever in the clear on the wire, in a log,
in a counter or in a report.

**Checked by.** Known-answer vectors for each format (built byte by byte in
the test, never from the writer), every refusal tested (bad signature, wrong
protocol id, wrong server key, nonzero pad, epoch 0 in a blob), and the
response-not-larger rule rechecked for type 10 (it is provoked by nothing; it
is sent on the server's own schedule, like a keep-alive).

### 2.3 L2: the server's keyed receive and the client's keyed send

**The server's receive check.** With the session layer on, the prefilter step
in `rocketnet_prefilter_check` (`sdk/rocketnet_protocol.cpp:50-70`) becomes
`rocketnet_filter_epochs_check` against the server's live table. Epoch 0 is
refused (STANDARD.md:536-539). The type and size rule is unchanged.

**The client's send.** The client writes its header with the epoch it was most
recently given (STANDARD.md:530-531) and the keyed value over that packet:
`rocketnet_write_prefilter_header` gains the epoch and the magic, and computes
the value after the four following bytes are written (for sealed packets that
is the sequence, for a request the first four bytes of the cookie field).

**The server's send: echo.** A server sends to a client at the epoch of the
last packet from that client that opened. It is written per slot, after the
open, exactly as the replay mark is, so a forged packet naming a different
epoch cannot move it. The client therefore never receives an epoch it does not
hold, and the server needs no record of what it has delivered. Handshake
replies (cookie, challenge, denied) echo the epoch of the request that provoked
them, which the client chose from its token.

**The client's receive check.** The client checks the server's packets against
its own table, so a client is no more exposed than a server. Whether this is
required or optional is QUESTION 11.

**Owns.** The server's live table, the per-slot echo epoch, the client's live
table and its send epoch.

**Guarantees.** Exactly one MAC per packet that reaches the keyed check. A
keyed receiver never accepts epoch 0. The server never sends an epoch the
client does not hold. The echo epoch moves only on an opened packet.

**Checked by.** `RNMagicRollout` (3.2), unit tests per refusal, and one
functional loopback case: connect at generation g, rotate to g+1 in session,
retire g, payloads flow throughout, and the counters show no keyed refusals.

### 2.4 L3: rollout and retirement

**The schedule.** The backend mints generation g+1 before g's end, publishes it
to every server in the fleet, and only after every server has installed it does
it name g+1 in tokens and tell servers to deliver it in session. A server
retires g when g is past its expire time and no slot's echo epoch is g. A slot
still on g at the forced retire (the table full, or g expired by more than a
grace period) is disconnected and counted
(`FILTER_GENERATION_RETIRED_CLIENT`), never left sending into a dead epoch.

**Owns.** The install API on the server
(`rocketnet_server_transport_install_filter_generation( transport, blob, bytes )`), the delivery schedule per slot, the retire rule.

**Guarantees.** Install before issue: no token names a generation its server
does not hold. No stranded client: no slot is left at an epoch the server has
retired. Every connected client reaches the newest generation, given that the
filter packet is eventually delivered.

**Checked by.** `RNMagicRollout` (3.2), whose backend side is the
extraction the owner confirms in QUESTION 2.

### 2.5 L4: the keyed filter in the kernel

**Owns.** A BPF array map of up to 4 `(epoch, magic)` entries written by
userspace on install and retire, and SipHash-2-4 over the 7-byte input in the
XDP program, verifier-clean.

**Guarantees.** The kernel rejects exactly what `rocketnet_filter_epochs_check`
rejects, never more: a generation is in the kernel map no later than in the
userspace table and leaves it no earlier. The same per-title generator emits
the constant, the userspace keyed reader and the BPF reader, so they cannot
drift (`sdk/rocketnet_wire.h:13-17` is the existing form of that promise).

**Checked by.** Host tier rows in `test_unit_xdp_filter_host.cpp` (keyed live,
keyed dead epoch, keyed wrong value, epoch 0 under keyed), the kernel tier
replaying the same rows, and a differential case: one corpus through both the
BPF reader and `rocketnet_filter_epochs_check`, every verdict equal.

### 2.6 L5: replay protection in the kernel

This layer needs no backend and no magic, and can be built in parallel with
L1-L4.

**Owns.** A BPF hash map keyed by client source address (the connection
identity, STANDARD.md:347-397), holding, for the session phase of the
client-to-server direction only: `hk`, a 64-bit high-water mark, and a ring of
the last 16 opened sequences. Userspace writes the entry after a session packet
opens and is marked, and deletes it when the slot is released, before that
address can be given a new mapping. The kernel only reads.

**The kernel's rule**, for session types only (4, 5, 6, 8, 9): reconstruct the
64-bit sequence against `hk` (STANDARD.md:747-753), then drop when the
sequence is at or below `hk - 4096` (the userspace window), or equals one of
the 16 recent sequences. Otherwise pass.

**Why it is safe.** `hk` is always at or below the userspace `highest_opened`,
because only an opened packet writes it, so every sequence below the kernel's
floor is also below the userspace floor; and every recent entry is a sequence
userspace has already opened and accepts only once. Every kernel drop is
therefore a packet userspace would refuse. A forger who wins the filter's odds
cannot move `hk`, so cannot poison a session by setting it to 2^32 (TODO.md's
safety condition). The kernel lags truth and is never corrupted by it.

**The tension, resolved as a proposal.** The owner prefers 16 entries, on
memory grounds; the standard requires the replay window to hold at least 4096
sequences (STANDARD.md:1608-1632, which TODO.md quotes as 256). Both hold when
the two are different things: **the 16 entries are an exact-duplicate cache in
the kernel, not a replay window**, and the 4096-sequence window remains
userspace's and the standard's. The kernel floor uses the userspace window's
size, never 16, so the kernel never refuses a delayed packet userspace would
accept. Per connection the kernel entry is 8 + 16 x 8 = 136 bytes, or about
136 KB at a thousand clients, against 512 bytes per connection for a 4096-bit
window in the kernel. The standard should say that a transport may hold a
smaller exact-duplicate cache in an early stage, and that the window rule
applies to the stage that marks (THE WIRE, W5).

**Checked by.** `RNKernelReplay` (3.4), host tier rows (duplicate dropped,
below floor dropped, in window passes, unknown address passes, after release a
fresh sequence 1 passes), the kernel tier on the same rows.

### 2.7 L6: identity, one seat per identity, and the kick

**The server's part, in the library.** A server reports connections and
disconnections and acts on a kick (STANDARD.md:986-993). The library gives it:

- an event surface: on connect, `(client_index, client_id, token identifier, user data)`; on disconnect, `(client_index, client_id, token identifier, reason)`; drained by the game each update, never a callback inside the
  transport's update;
- `rocketnet_server_transport_disconnect_client`, which exists, plus a guarded
  form keyed by the token identifier, so a kick for an old session can never
  end a newer one that reused the slot index.

**The backend's part: the seat table.** Per client id, the token identifier of
the seat it holds, and the server holding it. The messages are three, carried
by the title's own channel:

    Connected(server, client id, token identifier)
    Disconnected(server, client id, token identifier)
    Kick(server, client id, token identifier)

On `Connected` for an identity whose seat names a different token, the backend
sends `Kick` for the old token to the old server and records the new seat. On
`Disconnected`, the backend clears the seat only when it still names that
token, so a late report for an old session is harmless. A server acting on
`Kick` disconnects only a slot whose token identifier matches, and ignores the
rest. Messages may be late, duplicated or reordered; every rule is idempotent.

Keying by the token identifier rather than by the client id is the design's
one real choice: the client id is the same on the old and the new seat, so a
kick by client id is the kick that ends the new player. The token identifier is
already computed per request (`sdk/rocketnet_server_protocol.cpp:450-451`) and
names exactly one admitted connection (`docs/TLA-INVENTORY.md` M6,
`OneConnectionPerToken`).

**Guarantees.** A kick never ends the newest seat. A stale report never clears
a live seat. Once messages stop, each identity holds at most one seat. During a
handover, two seats can coexist for at most one report-and-kick round trip,
which is the bounded weakening of "one server at a time" this design accepts
and states.

**Checked by.** `RNSeats` (3.3), and a pure seat-table module tested with
reordered and duplicated message traces.

---

## 3. THE MACHINES

Each machine gets its TLA+ module before or alongside its code, checked with
TLC on a small instance, every counterexample verified against the code by
hand, and cited from the code that implements it.

### 3.1 RNFilterEpochs (exists as a card)

The receiver's generation table, L0. Variables `live` (sequence of epochs),
`newest`, ghost `macs`. Actions `Install(g)`, `Retire`, `Check(e)`. Invariants
`NeverKeyedAtZero`, `AtLeastTwoLiveAfterRotation`, `WrapSkipsZero`,
`DeadEpochRefusedWithoutMac`. Reversed witnesses `BrokenAcceptZero`,
`BrokenWrapToZero`. Card `rocketnet-tla-filter-epochs`, ready.

### 3.2 RNMagicRollout (new)

The generation's life from mint to retire, L1-L3, over one backend, two
servers and two clients.

- **Variables.** `minted` (generations the backend has made), `installed[s]`
  (the server tables), `issuing` (the generation tokens name), `token[c]` (the
  generations a client's token carried), `held[c]` (the client's table),
  `sendEpoch[c]`, `echo[s][c]` (the server's per-slot echo epoch),
  `connected[s][c]`, `inflight` (filter packets, lossy), and the clock.
- **Actions.** `Mint`, `Publish(s)` (a blob reaches a server, which installs),
  `StartIssuing(g)` (the backend names g in tokens), `Connect(c, s)`,
  `Deliver(s, c)` (a filter packet arrives), `Lose`, `ClientSwitch(c)`,
  `ServerOpen(s, c)` (writes the echo), `Retire(s, g)`, `ForceRetire(s, g)`
  (kicks slots on g), `Forge(s, e)` (a packet that fails the open).
- **Invariants.** `IssueAfterInstall`: `issuing = g` implies g is installed on
  every server. `NoStrandedClient`: a connected client's send epoch is live on
  its server. `EchoHeld`: the server's echo epoch for a client is in that
  client's table. `ForgeMovesNothing`: `Forge` changes no echo.
  `NoMagicAtZero` (from 3.1).
- **Liveness.** `EveryClientReachesNewest`, under weak fairness of `Deliver`
  and `ServerOpen`.
- **Reversed witnesses.** `BrokenIssueBeforeInstall` (fails
  `IssueAfterInstall`), `BrokenRetireWithClients` (fails `NoStrandedClient`),
  `BrokenSendNewest` (the server sends its newest rather than the echo; fails
  `EchoHeld`), `BrokenEchoOnReceive` (the echo written before the open; fails
  `ForgeMovesNothing`).

### 3.3 RNSeats (new)

The connected set across servers, L6, over two servers, one identity, three
tokens.

- **Variables.** `seat` (the backend's token per identity, or None),
  `slot[s]` (the token each server's slot holds, or None), `net` (a bag of
  messages, reorderable and duplicable).
- **Actions.** `Admit(s, t)` (a server admits token t and sends `Connected`),
  `Leave(s)` (a client disconnects; `Disconnected`), `BackendRecv(m)`,
  `ServerRecvKick(s, m)`, `Duplicate(m)`, `Drop(m)` for the lossy case.
- **Invariants.** `KickNeverEndsNewest`: no kick acted on removes the token
  `seat` names. `StaleReportHarmless`: a `Disconnected` for a token other than
  `seat`'s leaves `seat` unchanged. `OneSeatWhenQuiet`: when `net` is empty,
  at most one server holds the identity.
- **Liveness.** `HandoverCompletes`: with no `Drop`, after the last `Admit`
  only the newest token holds a slot.
- **Reversed witnesses.** `BrokenKickByClientId` (the server kicks by client
  id; fails `KickNeverEndsNewest`), `BrokenClearAny` (the backend clears on any
  `Disconnected`; fails `StaleReportHarmless`).

### 3.4 RNKernelReplay (new)

The kernel's read-only replay map beside the userspace window, L5. It extends
`tla/ReplayWindow.tla` rather than replacing it.

- **Variables.** The userspace window from `ReplayWindow` (`highest`, `bits`,
  ghost `opened`), the kernel entry `hk` and `recent` (at most `R` entries,
  R = 2 standing for 16), `present` (whether the address has an entry), and a
  ghost `kdrop` (sequences the kernel dropped).
- **Actions.** `Arrive(s)` (kernel verdict, then userspace check, open and
  mark), `Forge(s)` (passes the kernel, fails the open), `WriteBack` (userspace
  copies `highest` and the newest opened into the kernel entry), `Release`
  (the slot ends; the entry is deleted), `Reuse` (a new connection from the same
  address starts at sequence 1).
- **Invariants.** `KernelNeverRejectsAccepted`: every sequence in `kdrop`
  is one the userspace check would refuse at that moment.
  `KernelNeverAhead`: `hk` is at or below `highest`. `ForgeMovesNothing`:
  `Forge` leaves `hk` and `recent` unchanged. `ReuseStartsClean`: after
  `Reuse`, sequence 1 passes the kernel.
- **Reversed witnesses.** `BrokenKernelAdvance` (the kernel writes `hk` on
  receive; fails `ForgeMovesNothing` and then
  `KernelNeverRejectsAccepted`), `BrokenFloorAtRecent` (the floor is
  `hk - R`; fails `KernelNeverRejectsAccepted` on a delayed packet),
  `BrokenNoDeleteOnRelease` (fails `ReuseStartsClean`).

### 3.5 What is not a new machine

- The keyed check in the server and client receive paths is L0's `Check`
  called in one more place; it is covered by RNFilterEpochs plus RNMagicRollout.
- The kernel keyed filter is a stateless function of the kernel map, checked
  by the differential against L0, not by a model.
- The seat table's pure module is the code form of RNSeats' backend side.

---

## 4. THE WIRE

Every item here changes the wire or the standard and **needs the owner's
approval before any card that depends on it starts.** None changes the 6-byte
header, which is the property the 1.0 header was designed for.

| # | Change | Where | Why | Approval |
|---|---|---|---|---|
| W1 | The token tail: two `(epoch, magic)` pairs in the 64-byte pad outside the signature | STANDARD.md _Connect token format_ (897-916), _Where the keyed form comes from_ (558-601) | the only confidential path to a client at connect | owner |
| W2 | Session packet type 10, _connection filter_, server to client only, sealed, `[ack block][epoch][magic]` | STANDARD.md _Packets_, the size table and the response-size table (3578-3586) | delivering a generation inside a session | owner |
| W3 | The generation blob: signed by the backend, magic sealed to the server's X25519 key | a new STANDARD.md subsection under _The keyed filter value_ | a server's magic, with no backend-to-server shared secret | owner |
| W4 | The echo rule: a server sends at the epoch of the client's last opened packet; handshake replies echo the request's epoch | STANDARD.md _The keyed filter value_ | the server never sends an epoch the client lacks | owner |
| W5 | Window sizing: an early stage may hold a smaller exact-duplicate cache; the 4096 rule binds the stage that marks; an early stage never rejects what the full sequence accepts | STANDARD.md _Replay window sizing_ (1608-1632) and _Check placement_ (1694-1745) | resolves the 16 against 4096 tension | owner |
| W6 | The connected-set messages `Connected`, `Disconnected`, `Kick`, keyed by token identifier | STANDARD.md _What it requires, and where it lives_ (975-1022), as deployment guidance, not wire | the identity posture made concrete | owner |
| W7 | Public API: the server event surface, `install_filter_generation`, the guarded kick, and accessors for the token identifier and user data | `sdk/rocketnet.h`, `tools/exports.txt` | the library's half of L3 and L6 | owner (public surface) |

W5's _Check placement_ edit matters on its own: the current text says an early
stage MUST reject "exactly" what the full sequence rejects (STANDARD.md:1737-1741).
Any early stage that is a strict subset, which the kernel replay map is by
design, needs that sentence to read "no more than, with the userspace path
completing the rest", which is what the following sentences already argue.

---

## 5. THE CARDS

Ordered. Model cards are heavy and come before the code they cover; code cards
are pro, 1 to 4 files each, each naming its test. "After" lists true
dependencies only. W-numbers are the owner approvals a card waits for.

**In flight (not redesigned):**

0a. `rocketnet-keyed-filter-value` (pro, merging): the pure value.
0b. `rocketnet-tla-filter-epochs` (frontier, ready): RNFilterEpochs.
0c. `rocketnet-filter-epoch-window` (pro, after 0a, 0b): the receiver table.

**L1-L3, the rolling magic:**

1. `rocketnet-session-standard-text` (heavy; W1-W4, W6). STANDARD.md only:
   the token tail, type 10, the generation blob, the echo rule, and the
   connected-set messages, in the standard's own voice. Test: none, a standard
   edit; DONE-WHEN names the sections and the size tables recheck.
2. `rocketnet-tla-magic-rollout` (heavy; after 0b). `tla/RNMagicRollout.tla`,
   its MC wrapper, the control and four broken cfgs, README, CASES, RUNS,
   COVERAGE rows owed by cards 5-9.
3. `rocketnet-filter-generation-blob` (pro; after 1). New
   `sdk/rocketnet_filter_generation.{h,cpp}` and
   `test_unit_filter_generation.cpp`: write, verify then open; known answers
   and every refusal. Test `filter_generation_blob_known_answers`,
   `filter_generation_blob_refusals`.
4. `rocketnet-token-filter-tail` (pro; after 1). `sdk/rocketnet_connect_token.{h,cpp}`
   and `test_unit_connect_token_vectors.cpp`: the tail's writer and the
   client-side split. Test `connect_token_filter_tail_round_trip`, with a 1.0
   token reading as epoch 0.
5. `rocketnet-server-keyed-receive` (pro; after 0c, 2). `sdk/rocketnet_protocol.{h,cpp}`,
   `sdk/rocketnet_server_protocol.cpp`, a new test file: the server prefilter
   calls `rocketnet_filter_epochs_check` when keyed, refuses epoch 0. Test
   `server_keyed_receive_refusals`.
6. `rocketnet-server-install-generation` (pro; after 3, 5).
   `sdk/rocketnet.h`, `tools/exports.txt`, `sdk/rocketnet_server_transport.cpp`,
   a new test: the exported install. Test `server_install_generation_refusals`.
7. `rocketnet-client-keyed-send` (pro; after 0c, 4). `sdk/rocketnet_protocol.{h,cpp}`,
   `sdk/rocketnet_client_protocol.cpp`, a new test: the client writes its
   epoch and keyed value, and checks the server's. Test `client_keyed_send_value`.
8. `rocketnet-server-echo-epoch` (pro; after 2, 5). `sdk/rocketnet_server_protocol.{h,cpp}`,
   a new test: per-slot echo written after the open; sends and handshake replies
   use it. Test `server_echo_epoch_moves_only_on_open`, the
   `BrokenEchoOnReceive` trace replayed.
9. `rocketnet-filter-packet` (pro; after 1, 7, 8). `sdk/rocketnet_wire.h`,
   `sdk/rocketnet_protocol.cpp`, `sdk/rocketnet_client_protocol.cpp`, a new
   test: type 10 build and read, the client installs. Test
   `filter_packet_build_and_read`.
10. `rocketnet-filter-delivery-and-retire` (pro; after 9). `sdk/rocketnet_server_protocol.cpp`,
    `sdk/rocketnet_counters.h`, a new test: resend each interval until the echo
    moves, retire, forced retire counted. Test
    `filter_delivery_until_echo`, `filter_forced_retire_counts`.
11. `rocketnet-functional-keyed-rotation` (pro; after 10). One loopback case
    in `functional/`: connect at g, rotate, retire g, no keyed refusals.

**L4, the kernel keyed filter:**

12. `rocketnet-xdp-keyed-filter` (heavy; after 5). `server_xdp/server_xdp.c`,
    `sdk/rocketnet_wire.h`, `test_unit_xdp_filter_host.cpp`: the magic map and
    SipHash in BPF, verifier-clean. Test the four keyed host rows.
13. `rocketnet-xdp-keyed-differential` (pro; after 12). One corpus through
    the BPF reader and `rocketnet_filter_epochs_check`, every verdict equal.
14. `rocketnet-per-title-generator-keyed` (pro; after 12). The generator emits
    the keyed reader beside the constant; scope per QUESTION 9.

**L5, kernel replay (independent of L1-L4):**

15. `rocketnet-session-standard-window-text` (heavy; W5). STANDARD.md only.
16. `rocketnet-tla-kernel-replay` (heavy). `tla/RNKernelReplay.tla`,
    extending ReplayWindow; control and three broken cfgs.
17. `rocketnet-xdp-replay-map` (pro; after 15, 16). `server_xdp/server_xdp.c`,
    `sdk/rocketnet_wire.h`, `test_unit_xdp_filter_host.cpp`: the read-only
    check. Test the five replay host rows.
18. `rocketnet-replay-writeback` (pro; after 17). `sdk/rocketnet_server_socket_linux.cpp`,
    `sdk/rocketnet_server_protocol.cpp`, a new test: write after mark, delete
    on release before reuse. Test `replay_writeback_order`, the
    `BrokenNoDeleteOnRelease` trace replayed through a fake map.

**L6, identity:**

19. `rocketnet-tla-seats` (heavy). `tla/RNSeats.tla`; control and two broken cfgs.
20. `rocketnet-server-session-events` (pro; after 1, W7). `sdk/rocketnet.h`,
    `tools/exports.txt`, `sdk/rocketnet_server_transport.cpp`, a new test: the
    event queue, token identifier and user data accessors. Test
    `server_session_events_drain`.
21. `rocketnet-guarded-kick` (pro; after 19, 20). The kick by token
    identifier. Test `guarded_kick_ignores_new_seat`, the
    `BrokenKickByClientId` trace replayed.
22. `rocketnet-seat-table-pure` (pro; after 19; where it lives per
    QUESTION 7). The backend seat table as a pure module. Test
    `seat_table_reordered_and_duplicated_traces`.
23. `rocketnet-roadmap-session-rows` (flash; after each layer locks). The
    ROADMAP.md row splits into one row per layer, each with its evidence.

---

## 6. QUESTIONS

For the owner. Each is a decision this design does not take alone.

1. **Approve W1-W7?** Each is listed in THE WIRE with its section. Cards 1, 15
   and 20 wait on them.
2. **The extraction.** The title's backend already runs the connected set and
   the kick. Does it key seats by the token identifier or by the client id, and
   does it kick on the new server's connect report or on token issue? If it
   kicks on issue, a token that is never used ends a live seat; this design
   kicks on the connect report. And how does it learn a server has died:
   heartbeat, lease, or the orchestrator?
3. **In-session delivery: transport type 10, or a message-layer control
   record?** Type 10 keeps the prefilter's secret inside the transport that
   uses it and serves transport-only users. A control record (a kind beside
   CONFIG, STANDARD.md:2203-2227) adds no packet type, but makes the keyed
   filter depend on the message layer. This design proposes type 10.
4. **The magic is fleet-wide shared state.** The standard declines fleet-wide
   secrets elsewhere (no stateless reset, STANDARD.md:3753-3759), and the magic
   is one. This design accepts it because the magic guards only the filter:
   its compromise degrades a deployment to what epoch 0 already gives, and
   rotation bounds it. Is that the owner's reading? The alternative for
   session packets is a per-connection filter key derived from the session
   keys, held in the same kernel map as L5, which needs no rotation at all;
   only the connection request would still need a fleet magic.
5. **W5's wording.** Is "an early stage rejects no more than the full sequence"
   the intended reading of _Check placement_?
6. **A partitioned server.** A server that cannot reach the backend cannot be
   told to kick, so two seats can persist for as long as the partition lasts.
   Should such a server end its identity-layer sessions after a lease, or is
   the bounded double seat accepted?
7. **Where the seat table lives.** In `sdk/` as an optional pure module, in a
   reference backend under `tools/`, or only in this document?
8. **Public user data and token identifier.** Neither is public today
   (1.3). Card 20 exposes both; is the token identifier acceptable as a public
   value (it is a hash of data already in the clear in every request)?
9. **The generator's scope.** "Every language" in TODO.md: which languages are
   in scope for the keyed reader now: C (userspace), BPF C, and which others?
10. **Defaults.** Four live generations (the epoch-window card's choice), and
    what generation lifetime and grace period?
11. **The client's keyed receive check.** Required, so a client is filtered as
    a server is, or optional, since clients are rarely the flood target?
