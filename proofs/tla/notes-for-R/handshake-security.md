# Notes for package R from M7 handshake-security

Branch `models/v1.1.x/handshake-security`. Everything here is for package R
to apply; this package did not edit `docs/roadmap.sexp`, `ROADMAP.md`,
`proofs/tla/README.md`, `docs/TLA-INVENTORY.md` or anything under `sdk/tests`.

## Roadmap entries (docs/roadmap.sexp item format)

MM095, the AEAD integrity-limit teardown:

```
  (item "integrity-limit-model" :group "proof-debt" :area "proofs"
   :title "No model of the AEAD integrity-limit teardown"
   :text "Client and server end a connection after 2^36 packets that failed to open (ROCKETNET_INTEGRITY_LIMIT, sdk/rocketnet_constants.h; rocketnet_client_protocol.cpp process paths; rocketnet_server_protocol.cpp idle and session paths). No TLA+ module has that transition: ClientConnection and ServerConnection deliver a packet under the wrong keys and discard it, and their READMEs say the integrity limit is not modelled. An observer who can send junk under the session's addresses can end an attempt or a connection this way. A model would add a failed-open counter per direction with a small limit, the teardown as an action, and a property that only failed opens advance it and that a connection under the limit is never ended by it, with a reversed witness that counts an opened packet."
   :date "2026-11-13" :origin "models correction wave MM095; proofs/tla/README-ClientConnection.md and README-ServerConnection.md, What the model abstracts away")
```

MM113, ClientSession trace binding:

```
  (item "clientsession-trace" :group "proof-debt" :area "proofs"
   :title "ClientSession has no trace walk"
   :text "proofs/tla/ClientSession.tla's per-connection inventory (what rn_client_connect, api.cpp 1556-1642, resets and keeps across a reconnect) is matched to the code by reading only. Bind it the way the other trace-checked layers are bound: the client session's random walk emits the inventory's variables at each connect, disconnect and failure, tlacheck trace checks each walk is a behaviour of the spec, and the check runs in CI beside the witnesses."
   :date "2026-11-13" :origin "models correction wave MM113; proofs/tla/ClientSession.tla")
```

MM094, an amendment to the existing `tlc-timeout-debt` item: its text names
six ClientConnection cases (LArbitrary, LCrash, SNone, SAlternate, SBurst,
SArbitrary). The current records have nine ClientConnection FAIL/124 cases:
those six plus SFirst, SFirstkind and SDelay, which became debt when the
early send let them reach connected (README-ClientConnection.md, the early
send section). Of the nine only `MCClientConnectionLCrash.cfg` is in
`proofs/tla/NIGHTLY-DEBT.tsv`, so the item's "the weekly tlc-debt.yml runs
each" does not hold for the other eight. `CrashedServerTimesOut` is checked
only by LCrash and has no PASS in RUNS.tsv or RUNS-HISTORY.tsv. Suggested
text for the ClientConnection clause: "ClientConnection LArbitrary, LCrash,
SNone, SFirst, SAlternate, SBurst, SArbitrary, SFirstkind and SDelay (only
LCrash is in NIGHTLY-DEBT.tsv; CrashedServerTimesOut has no completed
check)", and the count in the title raised to match.

## proofs/tla/README.md

- Line 21, "The three historical ClientConnection runs that did not finish
  remain explicit `bench` debt cases": the current records have nine
  ClientConnection FAIL/124 cases (list above). Suggested: "The nine
  ClientConnection runs that do not finish inside the 110 s case limit
  remain explicit `bench` debt cases".
- Lines 243-248 and 260-265, the debt table and its receipts: they list
  LCrash, SFirst, SAlternate, SBurst, SFirstkind and SDelay; SNone,
  SArbitrary and LArbitrary are also FAIL/124 in the current RUNS.tsv
  (110.266 s, 110.247 s and 110.140 s on aws-mut-3, 2026-11-13).

## docs/TLA-INVENTORY.md

- Line 29, server handshake and security tables: "prefilter epoch rotation"
  cites `RNFilterEpochs`, which is historical (its header and README now say
  so; the keyed prefilter check was removed and `rocketnet_prefilter_check`
  refuses every epoch but UNKEYED). Suggested: drop "prefilter epoch
  rotation" and `RNFilterEpochs` from the row, or mark it "(historical)".
  The parked branch `tools/v1.1.x/trace-layers-at-scale` (3d658b038)
  deletes the module in 9b95b845d; when it lands, drop it from the row.
- Line 70, client slots free stack: "no slot taken while held" under
  `ServerConnection` is true from this branch on (`HeldSlotNotReused`,
  reversed witness `MCServerConnectionBrokenHeldSlotTaken.cfg`); before it
  ServerConnection had no hold. No change needed once this branch lands.

## Cross-family rows

- MM102, `proofs/tla/COVERAGE.tsv`, the encryption-map row (line 82;
  `sdk/rocketnet_encryption_map.cpp`): the note says ServerConnection models
  the hashed table's layout (flat index cells, open addressing, linear
  probing, backward-shift deletion). ServerConnection's `smap` is a plain
  function from address to mapping (ServerConnection.tla, the `smap`
  variable and `TypeOK`); the layout is `RNOpenAddress`'s (the code already
  cites `RNOpenAddress.tla Insert` and `EraseStart`/`EraseStep` in
  rocketnet_client_slots.cpp and rocketnet_token_tables.cpp). Suggested:
  the row's module column becomes `RNOpenAddress` (or `ServerConnection,
  RNOpenAddress`), and the note's first sentence becomes "ServerConnection
  models the encryption map as a function from source address to mapping;
  RNOpenAddress models the hashed table's layout: ...".

## Late rows on these modules that belong to R or M14

- MA005: all 26 `MCRNSecurityTables*.cfg` (the 21 the row counted plus
  this branch's `BrokenOrderInPlace`, `BrokenTakeLiveHead` and `Race`, and
  two more) set `CHECK_DEADLOCK FALSE`, and all 26 CASES.tsv
  rows say `check`. What the directive does in each: TLC's deadlock check is
  off. Under `Spec` (14 cfgs) and `ClaimsSpec` (3) the clock stops at
  `MaxClock`, so every behaviour ends in a state with no successor, which a
  deadlock check would report; under `FloodSpec`, `FloodSpecWeakHonest`,
  `ChargeSpec` and `RepeatSpec` (9) the directive only turns the check off.
  The invariants and properties each cfg lists are checked either way.
  The deadlock column for all 26 should be `ignore-terminal` (R), and the
  checker rule is M14's. This branch did not change the column.
- MA006: ClientConnection and ServerConnection state their delivery
  fairness through named operators (`DeliveryFairTyped`). Both groups have
  temporal witnesses that fail under that fairness: ClientConnection
  `WOnceFirstkind`, `WOnceAlternate` and `WAnysend` (Connects);
  ServerConnection `BrokenIpv6OnlyMatch`, `BrokenOneFamilyServed`,
  `BrokenOnceKeepAlive`, `BrokenNoTimeout` and `BrokenNoDeadline`. The
  checker hole is M14's.
- MA032: `proofs/tla/RUNS-ServerConnection.tsv` is still present and still
  read by nothing; R deletes it.

## sdk comment re-citations for package C

Line numbers at this branch's head.

- `sdk/rocketnet_server_protocol.cpp`
  (rocketnet_server_protocol_process_connection_request, the work-budget
  comment, lines 868-870): "A dropped client's next
  request is processed in a later update (proofs/tla/RNSecurityTables.tla
  HonestRequest; ...". New text: "A dropped client's next request may be
  processed in a later update, if the flood leaves room in one: nothing
  reserves it, so that is an assumption about the environment
  (proofs/tla/RNSecurityTables.tla HonestRequest, FloodSpec's strong
  fairness; proofs/tla/ReceiveOrder.tla WorkBudgetOnlyForVerify)." (MM106)
- `sdk/rocketnet_client_protocol.cpp` (the comment above
  rocketnet_client_check_deadlines, lines 321-322): "Tick (TickRule with Erase
  TRUE): the deadline or the timeout goes through rocketnet_client_set_state,
  which zeroes the keys." New text: "... which runs
  rocketnet_client_connection_reset: the keys, the cookies, the client index
  and the replay window are zeroed." (MM097)
- `sdk/rocketnet_client_protocol.cpp` (above
  rocketnet_client_process_denied, line 798): "proofs/tla/ClientConnection.tla
  ClientRecvDenied". New text: "proofs/tla/ClientConnection.tla
  ClientRecvDenied: CONNECTION_DENIED through rocketnet_client_set_state,
  whose rocketnet_client_connection_reset zeroes the keys, the cookies, the
  client index and the replay window." (MM097)
- `sdk/rocketnet_client_protocol.cpp` (line 733, the small-order check in
  rocketnet_client_process_challenge): add "proofs/tla/ClientConnection.tla
  ClientRecvChallenge (SmallOrderServer): the challenge is ignored and the
  attempt is unchanged." (MM096)
- `sdk/rocketnet_client_protocol.cpp` (above
  rocketnet_client_open_session, line 944): after "ClientRecvSession
  (ClientRecvSessionRule with Erase TRUE: ...)" add "; a packet refused
  after its replay mark (bad ack block, wrong size, keep-alive index past
  the slot count) is ClientRecvSessionRule's AuthenticRefusals branch: the
  id is marked and nothing else changes". (MM096)
- `sdk/rocketnet_client_protocol.cpp` (rocketnet_client_process_ack, line 1270):
  add "proofs/tla/ClientConnection.tla ClientRecvSession (the \"ack\" type,
  ServerAcks): the standalone ack refreshes the receive clock and moves no
  state." (MM096)
- `sdk/rocketnet_server_protocol.cpp` (line 2168), before
  rocketnet_server_protocol_write_ack_if_due: add
  "proofs/tla/ClientConnection.tla ServerAck" to the existing
  RNEndpoint.tla WriteAck line. (MM096)
- `sdk/rocketnet_server_protocol.cpp`: the comment on the -1 from
  rocketnet_client_slots_assign in
  rocketnet_server_process_connection_response (lines 1237-1239) and the
  one before rocketnet_client_slots_publish at the end of
  rocketnet_server_protocol_update (lines 1972-1976): add
  "proofs/tla/ServerConnection.tla HeldSlotNotReused" to each. (MM100)
- `sdk/rocketnet_server_protocol.h` (the disconnect reason comment, lines
  170-171): "(TIMED_OUT,
  CLIENT_DISCONNECTED, SERVER_DISCONNECTED, NONE for the integrity limit)".
  The integrity-limit teardown passes PROTOCOL_VIOLATION
  (`rocketnet_server_protocol.cpp`, the integrity-limit teardown, lines
  1334-1340). New text: "(TIMED_OUT,
  CLIENT_DISCONNECTED, SERVER_DISCONNECTED, PROTOCOL_VIOLATION for the
  integrity limit)". One commit with STANDARD-ADDENDUM SA013. (MA016 part 2)
