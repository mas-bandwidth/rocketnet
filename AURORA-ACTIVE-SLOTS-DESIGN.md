# Aurora active-slot walks — foundation design (no implementation yet)

- Lane: rocketnet preview-3, "Johnny and Aurora" — Aurora: server slot walks.
  Spec: `/Volumes/nova/ai/shared/rocketnet-preview-3-LANES.md` (gate: rebased on
  `origin/main`, Linux Release unit+functional on a bench, CI green, Windows
  gate for Windows `sdk/` code; target: every server main call under 0.1 ms
  p99 at 1000 clients).
- Checkout: `/Volumes/nova/ai/aurora/working/rocketnet-aurora`, branch
  `aurora/active-slot-walks`, base `c15d41d55577ad1028b905b9f20f9790124dba02`.
- Status: SUPERSEDED in part by Glenn's RULE CHANGE (2026-10-23, LANES.md):
  profile worst case only (1000 connected, all active); active-slot lists
  dropped. The walk inventory (§1) and baseline (§9) stand. Implementation
  instead cuts per-slot cost at full load on update/send: (1) Observe skips
  its per-slot connection poll while the slot binding epoch is unchanged
  (client_slots binding_epoch + link epoch + transport accessor), and
  (2) send_packets asks the transport for remaining capacity once and counts
  frames down locally instead of per slot per round. Tests:
  tests/test_unit_worst_case_calls.cpp. Local Release unit + functional green
  on studio.local; bench worst-case before/after still owed. No TLA change:
  no state machine changed (Observe still polls on any binding change;
  send rotation order and fairness unchanged).
- Cut (2026-10-23): `rn_channels_end_update_sequenced` no longer scans
  `region->holds` of every connection per update. Tracking: an intrusive
  delivered-view list in `rn_region_t` (`sequenced_delivered_head/count`,
  threaded through each live delivered hold's `ordering`, which is dead
  while `used==2`), pushed where `used=2` is set in
  `rn_channels_drain_sequenced` and drained at EndUpdate; `rn_region_carve`
  (the reuse reset) clears head/count with the holds, so a reused slot
  starts clean with no stale index and no whole-region reinit beyond
  today's carve. Cleared set is exactly `{i : used==2}`, as before;
  reliable-sequenced path untouched. Tests:
  tests/test_unit_sequenced_delivered_views.cpp (view expiry across a
  boundary, exact cleared set over churn, empty-list O(1) at holds=4096,
  clean reset/reuse). Local Release unit + functional green on
  studio.local; hulk worst-case before/after still owed (main). TLA:
  model-preserving for `tla/RNChannel.tla` EndUpdate — the model does
  not name the delivered list (auxiliary state); EndUpdate still expires
  exactly the delivered batch/views and nothing else, so no model or
  CASES/COVERAGE change; cited as `// tla/RNChannel.tla EndUpdate` in
  sdk/rocketnet_channel_sequenced.cpp.

## 1. Walk inventory — every all-slot walk in server update/send paths

`ROCKETNET_MAX_CLIENTS` is 1000 (`sdk/rocketnet.h:93`). "Per-slot cost" is the
idle cost (slot free / nothing due), which is what an active-only walk removes.

### 1a. In scope — per-update / per-send, O(max_clients) regardless of load

| # | Site | Walk | Idle per-slot cost |
|---|------|------|--------------------|
| T1 | [rocketnet_server_transport.cpp](/Volumes/nova/ai/aurora/working/rocketnet-aurora/sdk/rocketnet_server_transport.cpp:2601) `rocketnet_server_transport_update` maintenance sweep | `for offset 0..max_clients`, slot `(first+offset)%max_clients`, cursor `maintenance_next_client` (:330) | `connected[]` read + modulo; if connected: `maintenance_due` → mapping lookup + keep-alive/ack-debt reads across slots/map/acks allocations |
| T2 | [rocketnet_server_transport.cpp](/Volumes/nova/ai/aurora/working/rocketnet-aurora/sdk/rocketnet_server_transport.cpp:2213) `rocketnet_server_stats_publish_and_submit` (called :2598) | `for slot=phase; slot<max_clients; slot+=phases`, 20/update (`ROCKETNET_CLIENT_STATS_SLOTS_PER_UPDATE`, constants.h:204) | Already phased: ~20 slots/update. `connected` check + `connection_ids[slot]` per visited slot |
| T3 | [rocketnet_server_replay_marks.cpp](/Volumes/nova/ai/aurora/working/rocketnet-aurora/sdk/rocketnet_server_replay_marks.cpp:85) and :122 `rocketnet_server_replay_marks_walk` (called transport :2647) | Two full `0..max_clients` loops | Only when `replay_mark` op exists (XDP). Forget loop: `has_mark` read; mark loop: `connected` check |
| M1 | [rocketnet_message_server.cpp](/Volumes/nova/ai/aurora/working/rocketnet-aurora/sdk/rocketnet_message_server.cpp:459) held sum in `rn_message_object_update` | `for i 0..num_connections: held += hot[i].held` | One 4-byte read per 32-byte hot record (32 KB touched for 4 KB of data at 1000) |
| M2 | [rocketnet_message_server.cpp](/Volumes/nova/ai/aurora/working/rocketnet-aurora/sdk/rocketnet_message_server.cpp:487) BeginUpdate loop | `for i 0..num_connections`: `rn_endpoint_begin_update` + memset views/cursors + KICKED check | Endpoint call + two memsets (delivery-entries bytes; channels×4 bytes) **even for FREE slots** |
| M3 | [rocketnet_message_server.cpp](/Volumes/nova/ai/aurora/working/rocketnet-aurora/sdk/rocketnet_message_server.cpp:539) Observe loop (RNSlots Observe) | `for i 0..num_connections`: `link.connection(transport,i)` + `rn_slots_observe` | Function-pointer call per slot into transport (`connection_ids[slot]` read) + compare/write `changed[slot]` |
| M4 | [rocketnet_message_server.cpp](/Volumes/nova/ai/aurora/working/rocketnet-aurora/sdk/rocketnet_message_server.cpp:574) Tick loop | `for i...: if state!=CONNECTED continue; rn_endpoint_tick` | Hot `state` read per slot (32-byte stride) |
| M5 | [rocketnet_message_server.cpp](/Volumes/nova/ai/aurora/working/rocketnet-aurora/sdk/rocketnet_message_server.cpp:599) config-tick/kick loop | Same scan; per connected: `rn_endpoint_config_tick` + `rn_endpoint_error` | Hot `state` read per slot |
| M6 | [rocketnet_message_server.cpp](/Volumes/nova/ai/aurora/working/rocketnet-aurora/sdk/rocketnet_message_server.cpp:891) `rn_message_object_send_packets` rotation | `do { for offset 0..num_connections (from send_next_connection) } while (progress)` | Hot `state` read per slot per round; idle = one full scan, busy = one scan per round |

### 1b. Already active-only (the pattern to copy)

- `rocketnet_server_protocol_update` ([rocketnet_server_protocol.cpp](/Volumes/nova/ai/aurora/working/rocketnet-aurora/sdk/rocketnet_server_protocol.cpp:1469))
  iterates live encryption mappings via `rocketnet_encryption_map_next`
  ([rocketnet_encryption_map.cpp](/Volumes/nova/ai/aurora/working/rocketnet-aurora/sdk/rocketnet_encryption_map.cpp:241)):
  O(active) work. Caveat: the iterator still scans `live[]` linearly, so a
  sparse map costs O(capacity) cheap int checks — acceptable, and the model
  for "scan cheap, work only on live".
- `rocketnet_server_transport_send_packets_impl` (:3301) walks the payload
  ring's committed packets, not slots. Not a slot walk.
- Receive loops (:2340, :2477) walk received packets, not slots.

### 1c. Out of scope (create-time or single-slot; leave alone)

- Create-only O(N) init: `rocketnet_client_slots_create` memsets
  (client_slots.cpp:97–114), `rocketnet_server_acks_create`
  (rocketnet_ack.cpp:973), stats-worker wiring (transport :549, :1930),
  message-object per-connection init (message_server.cpp:291).
- Secondary, note-only: `rn_message_object_next` (:928) scans connections
  from `receive_connection` per receive call; worst O(N) when the queue is
  empty. Revisit only if the perf report names receive.
- `rocketnet_server_update_client_stats` (:2138) is a test-hooks-only fixture
  mirroring T2; update it only if T2's shape changes.

Per-slot state today: `rocketnet_client_slots_t` is already per-field SoA
(separate `client_id[]/connected[]/mapping_index[]/confirmed[]/user_data[]`,
client_slots.cpp:22–43); the message object has a 32-byte hot record array
`hot_connections[]` (connection_id, views*, cursors*, held, state;
message_object.h:134–148) beside cold `connections[]` (full endpoints). There
is no `netcode_server_t` in tree (only netcode.io behaviour comments); the
lane's "like netcode_server_t" means per-field arrays, which §3 follows.

## 2. Layers, bottom up (each secured before the next)

0. **Active-set primitive.** One intrusive doubly-linked list per slot
   universe: `next[]/prev[]` int arrays + `head/tail/count` + rotation cursor
   (a slot id, not an index). O(1) insert/remove, no allocation after create,
   no per-slot region work on insert/remove. Two instances: transport slots
   (owned with `rocketnet_client_slots_t`) and message connections (owned by
   `rn_message_object_t`). Client object (1 connection) degenerates to a
   0/1-element list; behaviour unchanged.
1. **Message-object walks (M1–M6)** on the message active set + a
   pending-changed queue for Observe (§4). Largest, most self-contained win;
   unit-testable through the existing in-memory link fakes with no sockets.
2. **Transport walks (T1–T3)** on the transport active set. T1 keeps its
   frame-exhaustion break and cursor fairness; T2 rebases its 20/update phase
   onto the list (next 20 active from a submit cursor); T3's forget loop
   switches to a marked-slots list (a slot that disconnects leaves the active
   set but must still be forgotten), mark loop walks active.
3. **Hot/cold field split** where profiles still show misses: split the hot
   record into per-field arrays for the scan loops (`state[]`,
   `connection_id[]`, `held[]` as separate arrays; `views[]/cursors[]`
   pointer arrays stay per-slot but are touched only for active slots).
   Transport side already SoA; only consider extracting ack hot fields
   (`ack_debt`, `keep_alive_acked`) if T1 remains hot after layer 2.

## 3. Active-list structure and invariants

- Insert at **tail** (FIFO claim order → rotation fairness is claim order);
  remove via prev/next in O(1); iteration caches `next` before the body so a
  kick/removal inside the body is safe; removals of *other* slots during
  iteration are impossible on the main thread (single-threaded update/send
  contract) — asserted, not locked.
- Invariants: (a) list membership ⇔ slot bound (message: state CONNECTED or
  KICKED-awaiting-reset; transport: `connected[]` set); (b) no duplicates,
  `count` = list length; (c) cursor always names a member or "wrap to head";
  (d) claim/release touch only that slot's links — **no whole-region or
  whole-array reinitialisation**, preserving today's O(1)
  `rocketnet_client_slots_assign/release` (client_slots.cpp:172, :260) and
  per-slot resets in `rocketnet_connection_reset.cpp:61–119`.
- Kicked slots stay in the message list until the next update's BeginUpdate
  ResetFree removes them — exactly today's deferred reset, so held views stay
  valid until that update (RNSlots Kick/ResetFree).

## 4. Observe semantics (the one walk that cannot just go active-only)

Observe must notice a free slot becoming bound, so walking only the active
set would miss connects. Fix: a **pending-changed queue** fed by the events
that change a slot's transport binding — `client_slots_assign`,
`client_slots_release`, and protocol reset — each appending the slot once
(dedup bit per slot; multiple churns within one update collapse to the final
binding, which is exactly what today's once-per-update read of the final
owner already observes — RNSlots models this: "several connect/disconnect
steps may precede a single Observe"). Observe drains the queue: per entry,
read `connection_ids[slot]`, call `rn_slots_observe`, keep RESET/DROP
handling byte-identical. KICKED slots enter BeginUpdate through the same
queue (kick appends), so M2 also walks only active ∪ kicked ∪ changed.

## 5. Maintenance rotation fairness

T1 today sweeps all slots each update and only stops early when the send
frame pool is exhausted; `maintenance_next_client` then resumes fairly.
Active version: start at the cursor slot, follow the list through one full
cycle (wrap at tail→head), same break on frame exhaustion, cursor = next
active slot after the last served. Every continuously-connected, due slot is
served within one cycle; a slot claimed mid-cycle joins at the tail and is
reached in that cycle or the next — the TLA liveness property in §6 states
this bound. M6 send rotation keeps "one frame per connection per round,
cursor = next unserved" with the cursor as a slot id over the list.

## 6. TLA+ plan (standing practice: model beside code, TLC small, cite it)

New module `tla/RNActiveSlots.tla` (extend-in-place of RNSlots was rejected:
RNSlots' `changed[]`/Observe is the contract M3 preserves; the list/cursor
machinery is a new machine). It refines RNSlots by mapping: list membership
↔ `state[s] ∈ {Connected, Kicked}`, queue drain ↔ Observe.

- Variables: `member[s]`, `order` (sequence of slots), `cursor`, `pending`
  (set of slots), `bound[s]` (connection id or 0), `served[s]` (last cycle).
- Actions: `Claim(s)`, `Release(s)`, `Churn(s)` (claim+release before
  Observe), `ObserveDrain`, `MaintenanceCycle`, `SendRound`, `Kick(s)`,
  `ResetFree(s)`.
- Invariants: `ListIsBoundSet`, `NoDuplicate`, `PendingCoversChurn`
  (every binding change since last drain is in `pending`), `CursorIsMember`.
- Liveness (WF on cycles): `EveryDueServed` — a continuously member, due
  slot is served within one full cycle; `NoStarvation` for send rounds.
- Broken witnesses (must fail, in the repo's reversed-witness style):
  release forgetting `pending`; head-insert starving the tail; cursor left
  dangling after its slot's release.
- Run TLC on SlotCount=3, MaxConn=2 on a bench (not the working Mac);
  register configs in `tla/CASES.tsv`, record runs in `tla/RUNS.tsv`, and
  cite `// tla/RNActiveSlots.tla <Action>` from each changed walk, matching
  the existing `// tla/RNSlots.tla Observe` comment style. Check every
  counterexample against the code by hand before writing it down.

## 7. Test plan (smallest focused, durable; no real time/sockets/network)

Unit tier only (`./bin/test`; time passed as an argument, in-memory link
fakes as in `tests/test_unit_message_objects.cpp` / `test_unit_slots.cpp`):

1. `tests/test_unit_active_slots.cpp` — the primitive alone: claim/release
   order, churn dedup in `pending`, remove-during-iteration, cursor wrap and
   fairness over sparse sets (e.g. slots {2, 500, 999} of 1000).
2. Message-object sparse test: 1000 connections, 3 bound via fake link;
   update + send_packets assert the 3 (and only the 3) get Observe/Tick/send
   treatment, kicks reset next update, held-view rejection still fires.
3. Maintenance fairness test at transport level via existing seams: due
   slots across frame-pool exhaustion are all served across updates, in
   rotation, with no slot served twice before all are served once.
4. Regression: full `./bin/test` + `./bin/functional` (Release, bench) and
   the reset-inventory tests unchanged unless §8 forces a decision entry.

## 8. Risks and coordination

- **Stella #527 (secrets-only destroy) — the main coordination point.** It
  touches create/reset/destroy. Rules for this lane: claim/release/Observe
  add only per-slot link writes; destroy frees the list arrays without
  changing #527's secret-wipe contract; create builds the empty list without
  per-slot region initialisation beyond today's. Land whichever of #527 /
  this PR merges second by rebasing; do not edit destroy wipe semantics.
- **Reset inventory** (`rocketnet_connection_reset.h`): adding fields to
  `rocketnet_client_slots_t` or the message hot/cold structs requires a
  layout-manifest entry plus a RESET/KEEP decision, or
  `test_connection_reset_inventory_covers_every_field` fails. Prefer list
  arrays as separate allocations beside inventoried structs; if a field must
  join an inventoried struct, it is KEEP (list links carry no secrets) with
  the decision recorded in the header.
- **Sizing:** message-object layout (`rn_message_object_layout`) and
  `rn_message_object_required_bytes` change if list arrays join the one
  block (R2: one allocation); required-bytes tests must move with it.
- **Iteration order changes** are observable (payload/ack admit order,
  replay forget-before-mark, stats phase order). Keep claim-order iteration
  everywhere and name any unavoidable order change in the PR.
- **Stats worker** spares/jobs are indexed by slot (transport :2222–2254);
  layer 2 must keep slot-indexed spares — only the *selection* of slots
  changes.
- **Client path:** `num_connections == 1` must stay a no-op behaviourally;
  the degenerate list is exercised by every existing client unit test.

## 9. Baseline (2026-10-23, studio.local, Apple M3 Ultra, arm64)

- Build: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DROCKETNET_XDP=OFF
  -DROCKETNET_IO_URING=OFF && cmake --build build -j8` — clean. Unit suite:
  `./bin/test` → `*** ALL TESTS PASSED ***`.
- Idle probe (`/tmp/idle_probe.c`, public API, portable transport, 0 clients
  connected, caller-supplied time; probe uses a loopback bind, so it is a
  baseline measurement, not a unit test): update+send p99 combined —
  10 slots 4 µs, 100 slots 9 µs, **1000 slots 33–38 µs** (update p99 31–36 µs,
  send p99 2 µs). So on this Mac the *empty* idle walk is already under the
  100 µs gate; the lane's ~0.5 ms figure must be the connected-idle and/or
  bench-CPU case, which this probe cannot reproduce without 1000 clients.
- Traffic datapoint (not the gate): `./bin/load 10 5` passes all criteria;
  `server_update_p99_us=220` at 10 clients with 1 msg/client/update each way
  on a shared Mac (msgx) — includes receive/process work; do not compare
  against the bench gate.
- **What the perf worker's report must provide** (vision, server alone on
  cores 32–63, clients on amd-3/hetzner2, Release): per-call p99 for
  `rocketnet_server_update` and `rocketnet_server_send_packets` at
  1/10/100/1000 **connected** clients, idle (connected, no traffic) and at
  the standard offered load, portable + gso, before this PR — then the same
  after each layer. That connected-idle 1000-client p99 is the number this
  design must move under 100 µs.

## 10. Exact next steps

1. Write `tla/RNActiveSlots.tla` + MC configs; TLC on a bench; log
   CASES/RUNS.
2. Layer 0+1: list primitive + message walks M1–M6 with test 1–2 above;
   `./bin/test` green locally.
3. Layer 2: transport T1–T3 with test 3; re-run idle probe + `./bin/load`.
4. Layer 3 only if the perf report still shows scan misses.
5. Rebase on current `origin/main` (watch #527), bench Linux Release
   unit+functional, Windows gate on captain for `sdk/` changes, then one PR
   (split after layer 1 if the diff grows large), merge own PR when green.
