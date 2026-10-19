# Deterministic simulation testing

One seed drives one single-process run of N clients and a server over the in-memory network
simulator, with loss, reorder, duplication, delay, clock skew and peer crashes all drawn from that
seed. Every check the run makes is a property a TLA+ model in `tla/` already states, named by
module and invariant, so a red run points at the clause it broke. Every failure prints its seed
and the command that replays it exactly. A per-push shard runs thousands of seeds inside the CI
budget; a nightly campaign runs a million or more.

This document is the design the five DST cards build from (section 8): four pro cards and one model card. It writes no code. FoundationDB's
simulation and TigerBeetle's VOPR are the references; no outside framework is adopted. Every number in
section 6 says whether it was measured or estimated.

## 1. What exists, and what the DST grows from

**`soak_sim.cpp` (the hostile soak).** Eight client protocol objects and one server protocol object
(`rocketnet_client_protocol_*`, `rocketnet_server_protocol_*`) through
`sdk/rocketnet_network_simulator.h` under mocked time stepped at 5 ms from 100.0, 2% loss, 2%
duplication, 50 ms latency and 20 ms jitter. It mints connect tokens (`rocketnet_write_connect_token`),
reconnects every client whose state falls to or below DISCONNECTED, disconnects each connected client
after `20 + 3c` simulated seconds, writes one pattern payload per client per step
(`generate_payload` / `verify_payload`) and a fragmented payload every 233 steps, verifies every
delivery at the server and the clients, fails on a terminal client state, treats a client that is
neither connected nor moving for 60 simulated seconds as a wedge (`HANG:`), and at the end checks
counter conservation (`rocketnet_counters_conserved`) and that every nonzero refusal counter is in
its explained table (`soak_explained_refusals`). `ROCKETNET_SOAK_SIM_SEED` pins the wire's seed, and
`./bin/soak_sim <n>` bounds it to n steps. It lacks: the message layer (it drives the protocol API
directly, so no channel type, ordering, receipt or reliable resend is exercised); a seed range in one
process; determinism beyond the wire (session keys and the cookie key come from `randombytes_buf`,
so packet bytes differ run to run while every packet's fate does not); any fault but the
simulator's four knobs; any check of order, exactly-once or session freshness; a trace hash.

**`sdk/rocketnet_network_simulator.h` (the wire).** Latency, jitter, loss and duplication under
mocked time, every random draw from the seed given at create. Delivery is "lowest occupied due slot",
not FIFO, so a jitter at or above the step reorders packets. `deliver_packets` scripts an exact
order; the audit (`rocketnet_network_simulator_audit`) and its negative control
(`rocketnet_network_simulator_inject_index_fault`) prove the indexed wire equals the reference scan.
It lacks per-packet extra delay, explicit reorder, per-peer clocks and peers that go away; section
3 builds all of these outside the simulator, so the simulator file is unchanged and every existing
user of it is untouched.

**`functional/channel_harness.h` (the message layer, one pair).** One client and one server
protocol object, each bound to a real SDK message object (`rn_message_object_create`) through the
`rn_message_link_t` seam (`rocketnet_channel_endpoint_update`, `_start_packet`, `_finish_packet`,
`_send_packets`, `_acked`, `_connection`, `_disconnect`), over the simulator, with time an argument
starting at 1000.0, `ROCKETNET_CHANNEL_SEED` choosing the seed (pinned `0x5EED5EED5EED5EED` when
unset) and an exit hook that prints the seed of a red case. It also scripts per-direction holds
(`ROCKETNET_CHANNEL_WIRE_HOLD_PAYLOADS`), counts allocations after create, and tees pcapng. It lacks
N clients (one client, slot 0, `connection()` answering 1), faults beyond the four knobs, and any
check but what each functional case asserts.

**`scripts/channel-seed-sweep` and `.github/workflows/nightly-channel-seeds.yml`.** Every
functional channel case over seeds 1-2000 each night, one process per (case, seed), one
`FAIL seed=<n> case=<name> reason=<what>` line per failure and the replay
`ROCKETNET_CHANNEL_SEED=<n> ./bin/functional --run <case>`. It lacks in-process seeds (a process per
run costs about 43 ms measured, section 6, so 2000 seeds of one case are a minute and a half), more
than one client, and checks beyond each case's own.

**`test_unit_sim_schedules.cpp` (scripted schedules).** Six replayable schedules (long skip then
stragglers, final fragment first, batch cap, same-tick slot reuse, sequence wrap, unreliable
aggregation), each composed as a list handed to `rocketnet_network_simulator_deliver_packets`. It is
exact and hand-written: it finds what it was written for and nothing else.

**`rocketnet_test_swarm` in `sdk/rocketnet_tests.cpp`.** A thousand client protocol objects and 50
garbage senders against one server on a direct wire (no simulator), with churn by role (stay, leave,
abandon mid-handshake), and exact counter accounting. It lacks the network, the message layer and
seeds.

**`scripts/netem-pair-soak` and `.github/workflows/nightly-soak.yml`.** The real thing: `bin/server`
and N `bin/client` processes in two network namespaces joined by a veth pair under netem, real
sockets, real threads, real time; nightly-soak also runs `soak_sim` unbounded-ish and sanitized. It is
neither deterministic nor replayable, and it is the only instrument that covers drivers, threads
and the kernel.

**`test/live_verify.h` (in flight, not on BASE at this writing).** The live-message verify scheme
shared by the examples and the loopback functional test: the sequence rides in the first bytes, the
length is a function of the sequence, byte i is `(i + sequence) % 256`, and a cycle byte names the
connection. The DST's payload scheme (section 2) is this scheme; when the header is on BASE the DST
includes it instead of carrying its own copy.

**The decision.** The DST grows from soak_sim's seeded bounded mode. Its world is soak_sim's world:
the same server protocol configuration, client addresses, connect token minting, reconnect rule
and end checks (conservation and the explained refusal table). Those four pieces move out of
`soak_sim.cpp` into `dst_harness.h` as functions and `soak_sim.cpp` calls them, so they exist once.
Each takes what differs as a parameter: the start time (soak_sim 100.0, the DST 1000.0, and the
cookie key rotation time is the start time plus `cookie_rotation_seconds`), the token's
`timeout_seconds` (soak_sim 10, the DST 5), and the source of key bytes (soak_sim
`randombytes_buf`, the DST its `keys` substream). soak_sim's protocol-direct traffic loop, its
pattern payloads, its 60-second wedge watchdog, profile and audit legs stay in `soak_sim.cpp`,
because driving the protocol API directly is what the soak is for; the DST's own watchdog is its
liveness checks (section 4). The DST adds, in `dst_harness.h`, the message layer
over the same `rn_message_link_t` seam the channel harness binds, written for N clients (the
server endpoint answers per slot), and the seed range, substreams, faults, checks and trace hash
below. `bin/dst` (`dst.cpp`) is its command line. soak_sim keeps its modes and its output: its
summary at `ROCKETNET_SOAK_SIM_SEED=1` is byte-equal at BASE and after the move, `bash
scripts/soak-sim-selftest` stays green, and `nightly-soak.yml` runs unchanged. `channel-seed-sweep`,
`nightly-channel-seeds.yml` and the functional cases are not touched: they are the per-type
functional tier and keep their one-pair harness. A later card may rebind `channel_harness.h` to the
DST's seam; until then the two seams implement one contract, `rn_message_link_t`, and the DST never
edits the functional harness.

## 2. The run

**The command.**

    ./bin/dst --seeds <a>-<b> [--shape push|full] [--clients <n>] [--steps <n>] [--faults <list>|all|none] [--max-fails <n>] [--verbose]

- `--seeds a-b` is required: an inclusive range of master seeds, decimal, `a <= b`.
- `--shape` picks the seed's size. `full` (the default): 2..8 clients, a fault window of 2000 steps
  (20 simulated seconds) and a quiet window of 2000 steps. `push`: 2..4 clients, a fault window of
  500 steps and a quiet window of 1500 steps; it is the per-push shard's shape (section 6). The shape
  is part of the seed's identity: a replay names it.
- `--clients n` pins the client count (1..16); without it each seed draws its own from the shape's
  range.
- `--steps n` overrides the shape's fault window, in steps (the minimiser's knob, section 6); the
  quiet window is the shape's.
- `--faults` names the fault kinds that are on, comma separated from `loss,dup,jitter,reorder,delay,
  skew,crash-client,crash-server`; `all` is the default and `none` turns every injector off (the
  simulator's link is then lossless at the drawn latency). A kind that is off makes no draws, and its
  substream is never read, so the other kinds draw exactly what they draw with it on.
- `--max-fails n` stops after n failing seeds (default 20).
- `--verbose` prints `seed=<n> trace=<hash>` for every seed.
- Exit 0 when every seed passes, 1 on any failure, 2 on a usage error.

Every seed runs in the one process, one after another, each building and destroying its whole
world; nothing survives from one seed to the next but the allocator's count, which must return to
its level before the seed (a check, `leak`).

**One seed.** All times below are simulated seconds; the true clock is `t(k) = 1000.0 + k * 0.01`
at step k, computed from k and never accumulated.

- Clients: drawn uniformly from the shape's range (the `schedule` substream) unless `--clients`
  pins it. The server has 16 slots, as soak_sim's.
- Step: 10 ms. Each step, in this order: apply the fault events due at this step; update every
  live client message object at its own clock (section 3, skew) and pump its sends; update the live
  server message object at its clock and pump its sends; collect and check every delivery; run the
  per-step checks; append to the trace.
- Windows: the fault window is steps `[0, F)` and the quiet window `[F, F + Q)`, F and Q from the
  shape (`full` 2000 and 2000, `push` 500 and 1500). In the quiet window no new fault starts, the link returns to the seed's base
  latency with zero loss, duplication, jitter and extra delay, crashed peers have restarted, and
  skew keeps its rate (it is a property of the clocks, not an event). The liveness checks run at the
  last step.
- Connect: each client connects at a step drawn from `[0, 100)` (`schedule.c<i>`). A client whose
  transport falls to DISCONNECTED or below is reconnected with a fresh token the next step, as
  soak_sim does, except a client in its churn pause (next item), which reconnects when the pause
  ends; a client whose session ends during the run is a new session (a new generation, below).
- Churn: each client, with probability 0.2 (`schedule.c<i>`), disconnects cleanly once at a step
  drawn as a fraction of the fault window and pauses 50 steps before it reconnects.
- The world's draws are made in a fixed order and count whatever the flags say: `schedule` draws
  the client count from the shape's range first and `--clients` then overrides it, and every
  per-client draw comes from that client's own substream, so changing the count changes nothing
  any remaining client draws.
- Message config: `rocketnet_message_default_config` (as the channel harness takes it: a window of
  1024 messages, a receive ring of 262,144 bytes, `max_message_bytes` 262,144) with `num_channels =
  9`, on both ends, and the default send budgets.
- Traffic: nine channels. Channels 0-7 each carry one channel type, channel c carrying type c
  (`ROCKETNET_UNRELIABLE` 0 through `ROCKETNET_RELIABLE_ORDERED_WITH_ACK_RECEIPT` 7), so each
  per-channel check sees one type. Channel 8 is mixed: types 3, 4 and 1 interleaved, so the
  coupling of sequenced messages to the ordered epoch is exercised (section 4). Types 0 and 5 carry
  no channel on the wire and are received on channel 0 (docs/CHANNELS.md, "UNRELIABLE"); every
  check therefore keys a stream by the send channel in the payload (byte 5), never by the receive
  channel, and a check (`whole_message`) requires types 0 and 5 to arrive on receive channel 0 and
  every other type on its send channel. Each step, each connected end sends to each peer with
  probability 0.5 one message on a channel drawn uniformly from 0..8, its size drawn: 70% from
  14..256, 25% from 257..4000, 5% from 4001..16384, the last band only on reliable types (2, 3, 4,
  6, 7); unreliable types (0, 1, 5) cap every band at 1400 bytes, inside every one of their one-packet
  limits. An end does not send on a connection whose reliable bytes sent and not yet delivered at
  the peer (the checks' ghost state knows both) exceed 65,536, a quarter of the receive ring, so
  message memory is never exhausted by construction; a MESSAGE_MEMORY_EXHAUSTED disconnect is
  therefore a FAIL (`check=memory_exhausted`). A send the message object refuses (returns 0) is
  recorded as not sent and is not a failure. Draws come from `traffic.c<i>` for client i and
  `traffic.server` for the server, so changing the client count never changes what client 0 sends.
- Payload: the live-verify scheme. Bytes 0-3 the message's sequence on its (session, send channel,
  direction, type), counted from 0, little endian; byte 4 `0xA0 | direction << 3 | (type & 7)`;
  byte 5 the send channel; bytes 6-8 the session generation (24 bits, little endian), byte 9 the
  server incarnation the session was admitted under (8 bits); bytes 10-13 the ordered epoch, which on channel 8 is the number of type 3
  messages sent on that stream before this one (0 on other channels); then byte i is
  `(i + sequence) % 256`. The length is the drawn size, which the sender records per (stream,
  sequence) for the checker. Every size is at least 14 bytes.
- Session generation: a global counter, one value per client session, starting at 1. The client's
  token carries `client_id = generation * 256 + client_index`, so the server reads the session in a
  slot with `rocketnet_client_slots_client_id`. The server message object's `connection(i)` answers
  that client_id while the slot is connected and 0 otherwise; the client's answers its generation
  while CONNECTED. A server incarnation (section 3) is a separate counter, starting at 1, carried
  in byte 9, never merged into the generation word: the two fields are compared separately (the
  `stale_session_payload` row of section 4). Generation 0 never occurs (the counter starts at 1; the message object's `connection(i)` answers 0
  for "not connected"). The harness holds both counters, so a client stamps byte 9 with the
  incarnation current when its connect completed, and sends nothing before it is connected. A
  server crash destroys every session, so a live session's incarnation is always the server's
  current one. The harness asserts at allocation that `client_id >> 8` (the generation, in the
  token's 64-bit client_id) is below 2^24 and the incarnation below 256 (a seed's session count is
  bounded by the clients' connects, churn and crashes, and the server crashes at most once a seed,
  section 3, so the incarnation is at most 2); a violation is
  `FAIL check=dst_bound`, never a silent wrap. Both links set `clock_from_first_update = 0`, so each message object
  reads its peer's skewed clock exactly as it is passed (section 3).
- The seam: the full `rn_message_link_t` (`update`, `payloads`, `acked`, `max_payload_bytes`,
  `start_packet`, `finish_packet`, `abort_packet`, `send_packets`, `connection`, `disconnect`,
  `state`), one client endpoint per client and one server endpoint answering per slot, written to
  the same contract as `functional/channel_harness.h`'s endpoint and following its call order.

**The substreams.** One 64-bit master seed S. Every random choice draws from a named substream whose
starting state is `mix(S ^ fnv1a64(name))`, where `fnv1a64` is FNV-1a 64 over the name's bytes
(offset basis 0xcbf29ce484222325, prime 0x100000001b3) and `mix` is splitmix64's finaliser
(`z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9; z = (z ^ (z >> 27)) * 0x94d049bb133111eb; z ^ (z >> 31)`).
A draw advances the state by `0x9E3779B97F4A7C15` and returns `mix(state)`. A uniform integer in
`[lo, hi]` is `lo + draw % (hi - lo + 1)`; a uniform real in `[0, 1)` is `(draw >> 11) * 2^-53`.
The names are fixed strings, `<i>` a client index in decimal:

| Substream | Draws |
|---|---|
| `wire` | the simulator's own seed, passed to `rocketnet_network_simulator_create` (every loss, duplicate and jitter roll inside it) |
| `keys` | every byte `randombytes_buf` returns in `bin/dst` (below) |
| `schedule` | the client count |
| `schedule.c<i>` | client i's connect step and churn |
| `traffic.c<i>`, `traffic.server` | each end's sends: whether, channel, size |
| `link` | the seed's base latency (drawn even with every fault off: it is the link, not a fault) |
| `fault.loss` | the seed's loss rate |
| `fault.burst` | the loss bursts: whether, when, how long |
| `fault.dup` | the seed's duplication rate |
| `fault.jitter` | the seed's jitter |
| `fault.reorder` | per packet: whether it is held, and for how long |
| `fault.delay` | the delay spike: whether, when, how long, how high |
| `fault.skew` | each peer's clock offset and rate |
| `fault.crash.client` | client crashes: how many, and for each a client index in 0..15 (skipped and traced when not below the client count), when, how long down, same address or not |
| `fault.crash.server` | the server crash: whether, when, how long down |

A new fault kind gets a new name, so adding it reshuffles nothing that exists. A kind switched off by
`--faults` never reads its substream.

**The randomness of the SDK.** The SDK draws from `randombytes_buf` for session keys, salts, the
cookie key and the hash keys of its tables. `bin/dst` installs, before `rocketnet_init`, a
`randombytes_implementation` (libsodium's `randombytes_set_implementation`, which the vendored sodium
exports) that serves bytes from the `keys` substream, reset at the start of every seed, so a replay
is byte-identical down to the ciphertext. The trace hash never covers key, ciphertext or salt
bytes, so it is the same with or without that implementation installed; the unit tests in
`./bin/test` run without it.

**The trace hash.** FNV-1a 64 over fixed-width records appended in step order: every wire send
(step, sender index, receiver index, wire length, first byte, which is the clear packet type),
every injected fault event (step, kind, peer, the drawn values), every transport state change
(step, peer, new state), every delivery (step, receiving peer, send channel, type, sequence,
length) and every receipt (step, peer, serial, outcome). No payload, key, salt or ciphertext byte
is hashed beyond the first. The server is peer index 0, client i is i + 1. The run's trace is FNV-1a 64 over the
per-seed hashes in seed order.

**The output.** A passing range prints one line and exits 0:

    PASS seeds=<a>-<b> runs=<n> trace=<16 hex digits>

A failing seed stops at its first failing check and prints, then the range goes on to the next seed:

    FAIL seed=<n> step=<k> check=<name> what=<detail>
    replay: ./bin/dst --seeds <n>-<n> <every non-default flag of this run>

and the run ends with `FAIL seeds=<a>-<b> runs=<n> failed=<m>` and exit 1. The replay line repeats
the same FAIL line, step and detail included, because nothing in a seed depends on anything outside
it.

**What is never read.** No wall clock anywhere on the DST path (`rocketnet_platform_time` is not
called; soak_sim's profile stays in soak_sim), no socket, no thread, no file. `rocketnet_init` runs
once per process and reads `ROCKETNET_LOG_LEVEL`; `bin/dst` then sets the level to NONE, as
soak_sim does, and reads no other variable. Floating point is built with `-ffp-contract=off`, as the whole tree is.

## 3. The faults

Every fault draws from its own substream (section 2) and is written to the trace when it fires.
Every fault lies wholly inside the fault window: a fault with a duration D starts at a step drawn
uniformly in `[0, F - D)`, after D is drawn, and every duration bound below is also capped at
`F / 5` steps (100 in `push`, 400 in `full`), so each fault ends before the quiet window starts. Rates below are the defaults per seed; each is a draw, so a
range of seeds covers the range of rates.

**Loss, duplication, jitter (the simulator).** At seed start `link` draws the base latency
uniformly from 5..150 ms, `fault.jitter` the jitter from 0..(latency / 2), `fault.loss` the loss from
{0, 1, 2, 5, 10, 20}% with weights {10, 30, 30, 15, 10, 5}, and `fault.dup` the duplication from {0,
1, 2, 5}% with weights {40, 30, 20, 10}; a kind that is off is 0 and draws nothing;
`rocketnet_network_simulator_config` applies them. Up to two loss bursts (`fault.burst`, each with
probability 0.5) raise loss to 100% for 20..200 steps, then restore it. The quiet
window sets loss, duplication and jitter to 0 and keeps the base latency.

**Reorder and delay (outside the simulator).** The simulator computes delivery as `current_time +
latency + jitter`, so the DST delays a packet by passing a later `current_time` to
`rocketnet_network_simulator_send`. Reorder: each packet, with probability drawn per seed from {0,
1, 5}% (`fault.reorder`), is sent with `current_time + d`, d uniform in 10..100 ms, so packets sent
after it overtake it. Delay spike: with probability 0.3 per seed (`fault.delay`), one spike adds
500..1500 ms to every packet in both directions for 50..300 steps. The 1500 ms tail of a spike's
last packet is inside the quiet window's first two seconds, which the liveness bound covers. Neither changes the simulator.

**Clock skew.** Each peer p draws once per seed (`fault.skew`) an offset `o_p` uniform in
[-0.25, +0.25] s and a rate `r_p` uniform in [0.995, 1.005]. The time passed to that peer's update
(and every time it reads: its message object's `update` and its protocol's timestamps) is
`c_p(k) = 1000.0 + o_p + r_p * (t(k) - 1000.0)`, so it never runs backwards and stays within
`|c_p - t| <= 0.25 + 0.005 * (t - 1000.0)`, which is at most 0.45 s over a default 40-second seed.
The wire always runs on the true clock: the seam passes `t(k)` to every
`rocketnet_network_simulator_send` and `_receive`, whatever time its peer's update was called with.
The unix timestamp the server checks tokens against is `1700000000 + floor(c_0(k) - 1000.0)`, and
tokens are minted on the true clock, so skew shifts token checks by under a second against a
300-second token life. The server is peer 0 and is skewed like any client.

**Client crash and restart.** `fault.crash.client` draws a crash count from {0, 1, 2} with weights
{50, 35, 15}, and for each a client index, a down time of 10..300 steps, and a crash step.
A crash destroys the client's message object and protocol object at once, sending nothing: no
disconnect burst. While down the client is not updated, sends nothing, and every packet the
simulator holds for its address is received and discarded each step (counted, traced). Restart
creates a new protocol object and message object, mints a token for a new generation and connects;
with probability 0.5 it keeps its address, otherwise it takes the next unused port, as a process
that rebinds does. The server learns of the crash only by its timeout: tokens carry
`timeout_seconds = 5`. A crash due while a client is already down is skipped and traced as skipped.

**Server crash and restart.** With probability 0.2 per seed (`fault.crash.server`) the server
stays down 10..200 steps after a crash at a step drawn in `[F / 5, F - D)`. A crash destroys the server
protocol object, its encryption map, used-token history, client slots, acks and reassembly, and its
message object, sending nothing; packets to the server's address are discarded while it is down.
Restart builds all of them fresh, as at seed start, with a new cookie key from `keys` and a new
server incarnation. Packets of the old sessions still in the wire reach the new server and are
refused there. Those counters are `PAYLOAD_NO_MAPPING`, `KEEP_ALIVE_NO_MAPPING` and
`DISCONNECT_NO_MAPPING`, which are EXPECTED-class and need no row, and
`CONNECTION_RESPONSE_NO_MAPPING`, a refusal, which the explained table gains as "server restart or
client crash: a handshake answer for a mapping that is gone". Any other refusal counter that a run
of correct code shows is reported by the card that sees it as a finding with its seed; it is never
added to the table without one. Clients see the restart as silence: each times out (5 s) and
reconnects with a fresh token.

**What a crash keeps.** Nothing of the crashed peer's memory. The world keeps the peer's index, its
address unless the restart draws a new one, its clock (offset and rate), its traffic substream, and
the checks' ghost state (what was sent and delivered on its old sessions), which is what the stale
session checks read.

**Terminal states.** A client reaching CONNECTION_TIMED_OUT, CONNECTION_RESPONSE_TIMED_OUT,
CONNECTION_REQUEST_TIMED_OUT or COOKIE_REQUEST_TIMED_OUT is explained during the fault window and
for 1000 steps after it, and reconnects; after that it is a FAIL (`check=client_terminal`).
CONNECTION_DENIED is explained only within 500 steps after that client restarted at its old address
(the server may still hold the old session); INVALID_CONNECT_TOKEN and CONNECT_TOKEN_EXPIRED are
always a FAIL.

## 4. The checks

Each check runs where the table says, prints `FAIL seed=<n> step=<k> check=<name> what=<detail>`
on its first violation, and carries a comment naming the module and invariant it mirrors as written
in `tla/*.tla`. The checks observe deliveries, receipts, link acks, transport states and counters;
none changes SDK behaviour. "Session" below is (client index, generation) on the client side and
(slot, client_id) on the server side; "stream" is (session, channel, direction).

| Check | Mirrors (module: invariant) | The runtime check | Where it hooks |
|---|---|---|---|
| `one_session_per_slot` | ServerSession: `SlotOwnsItsSession`; ClientSession: `CurrentOnlyWhileLive` | No two connected slots hold one client_id; a generation is connected in at most one slot; a client's message object reports a connection only while its transport is CONNECTED | every step, after the server update |
| `stale_session_payload` | ServerSession: `SlotReuseObservesNothing`; ClientSession: `ReconnectObservesNothing`; RNSlots: `ReusedSlotStartsClean`; RNSimFaults: `DeliverOnlyToLiveGeneration` (section 7) | Every delivery's generation (bytes 6-8, read as a 24-bit value) equals the receiving session's, and its incarnation (byte 9) equals the receiving session's incarnation, the two compared separately: at the server, generation against `rocketnet_client_slots_client_id(slot) >> 8` and incarnation against the server's current one; at a client, its current generation and the incarnation of the server it connected to. No payload of a crashed, disconnected or timed-out session is delivered after its session ends | every delivery |
| `token_admits_once` | ServerSession: `TokenAdmitsOnce` | Each client_id reaches connected at most once per server incarnation | every server slot change |
| `client_terminal` | ClientSession: `ConnectReachesConnectedOrError` (safety side) | A terminal client state outside the explained windows of section 3 | every client state change |
| `whole_message` | RNChannel: `WholeMessagesOnly`; FragmentsAndAcks: `DeliveredExact` | The delivery's length and every byte match the payload scheme for its sequence and the size recorded when it was sent | every delivery |
| `reliable_once` | RNChannelTypes: `ReliableExactlyOnce`; RNEndpoint: `ReliableExactlyOnce`; FragmentsAndAcks: `DeliveredOnce` | On types 2, 3, 6, 7 (and 3 on channel 8), no sequence of a stream is delivered twice, and no sequence that was never sent is delivered | every delivery |
| `ordered_in_order` | RNChannel: `OrderedExactlyOnceInOrder`; RNChannelTypes: `OrderedExactlyOnceInOrder`; RNEndpoint: `OrderedInOrder`; RNWindow: `OrderedDeliveredInOrder` | On types 3 and 7, and type 3 on channel 8, the k-th delivery of the (stream, type) has sequence k, counting from 0 (sequences count per type, section 2) | every delivery |
| `sequenced_forward` | RNChannelTypes: `SequencedNeverBackwards`, `NoSequencedDuplicate`, `SequencedNewestWins` | On types 1 and 4: sequences strictly increase on the stream. On channel 8: for deliveries i before j of types 1 and 4, epoch(i) < epoch(j), or the epochs are equal and seq(i) < seq(j) | every delivery |
| `ordered_unchanged_by_sequenced` | RNChannel: `OrderedTraceUnchangedBySequenced`; RNChannelTypes: `OrderedTraceUnchangedBySequenced` | On channel 8, the ordered deliveries alone are the ordered sends in order, exactly as `ordered_in_order` requires of channels 3 and 7 | every delivery |
| `unreliable_once` | RNChannel: `UnreliableAtMostOnce`; RNEndpoint: `UnreliableAtMostOnce` | On types 0 and 5, no sequence of a stream is delivered twice | every delivery |
| `acked_means_opened` | RNReceipts: `AckedMeansPacketOpened`; RNChannelTypes: `AckedImpliesTransportArrival`; ReplayWindow: `AckReportsOnlyReceived`; FragmentsAndAcks: `AckedMeansSurfaced` | Every packet id a link reports through `acked()` maps (by the seam's id-to-sequences record, made at `start_packet` and `finish_packet`: every transport sequence the packet's frame was sealed under, more than one when the payload is fragmented) to sequences that the peer session all opened. The opened set is kept by the seam: at a client, `received_session_sequence` after each accepted `rocketnet_client_protocol_process_packet`; at the server, the mapping found with `rocketnet_encryption_map_find( server->encryption_map, &from )` and read with `rocketnet_encryption_map_entry`, and the one sequence that changed in its `replay_session` across `rocketnet_server_protocol_process_packet` (the new `highest_opened` if it rose, else the one bit gained in `window`) | every link `acked()` call |
| `receipt_one_outcome` | RNReceipts: `ExactlyOneOutcomePerReceiptSerial`, `LossOnlyForUnreliableReceipt` | Each receipt serial of types 5, 6, 7 gets exactly one outcome; LOSS only on type 5 | every receipt taken |
| `receipt_acked_delivered` | RNReceipts: `BlockAckedOnlyWhenAllFragmentsAcked`; RNChannelTypes: `EveryReliableAckedAndDelivered` (safety side) | An ACKED receipt for a type 6 or 7 message names a message the peer has delivered, or, for type 7, one whose stream still has an earlier sequence undelivered at the peer (it is held behind that gap); by the last step, on a live session, delivered | every receipt taken; again at the last step |
| `replay_monotone` | ReplayWindow: `Monotone`, `HighestIsReceived` | Within a session, a receiver's `replay_session.highest_opened` never decreases | every accepted packet |
| `conservation` | (no model: the counter law of `rocketnet_counters_conserved`) | Server and client counters conserve, as soak_sim checks | the last step |
| `unexplained_refusal` | (no model: soak_sim's explained table) | Every nonzero refusal counter is in the explained table, which gains the one row of section 3 | the last step |
| `crashed_silent` | RNSimFaults: `CrashedPeerSilent` (section 7) | A crashed peer is never updated, sends nothing, and receives nothing (every packet for it is discarded) | every step |
| `skew_bounded` | RNSimFaults: `SkewBounded`, `ClockMonotone` (section 7) | Every peer's clock is within its bound of the true clock and never below its value the step before | every step |
| `fresh_generation` | RNSimFaults: `RestartIsFresh` (section 7) | A restarted client connects only under a generation never used before | every client connect |
| `leak` | (no model) | The allocator's count after the seed's destroy equals its count before the seed's create | seed end |
| `liveness_connected` | ClientSession: `ConnectReachesConnectedOrError`; ServerSession: `HandshakeResolves`; RNSimFaults: `EveryLiveClientConnects` | Every client that is not crashed is CONNECTED and the server holds its session, by the last step (the quiet window: 20 s in `full`, 15 s in `push`, after the fault window) | the last step |
| `liveness_delivered` | RNChannel: `EveryOrderedMessageDelivered`; RNChannelTypes: `EveryReliableAckedAndDelivered`; RNReceipts: `ReceiptProgress` | Every message of types 2, 3, 6, 7 sent on a session still live at the last step is delivered; for type 4 the stream's last sent sequence is delivered; every receipt serial on a live session has its outcome | the last step |

Not mirrored at runtime: ReceiveOrder's `CheapRefusalsFirst` (the order of the server's receive
rungs is not observable from outside the protocol object; its unit tests hold it), RNReceipts'
`UnreliableNeverResent` (a packet's contents are not decoded by the harness), and the TypeOK and
bound invariants (RNChannel and RNChannelTypes `BuffersBounded`, RNChannel and RNWindow
`NoSlotCollision`, FragmentsAndAcks `ArenaBounded`), which are internal to the objects. The liveness bound is the quiet window: the client timeout (5 s) plus the handshake deadline (5 s)
plus a reconnect and resend cycle under the base latency, which `push`'s 15 s covers once and
`full`'s 20 s covers with room. A delay spike or a crash is always inside the fault window, so it
is over when the bound starts.

## 5. The proof: two planted bugs

A DST that has never found a bug is not known to find one. Two bugs are planted, each an exact-once
source replacement applied to a COPY of one SDK file under `build/`, built into a separate binary
`dst_planted`; no product branch and no `#ifdef` in `sdk/` ever carries a plant.

**The ordering plant.** File `sdk/rocketnet_channel.cpp`, function `rn_channels_dispatch_ordered`.
Replace the one line

    if ( delta == 0 )

with

    if ( delta <= 1 )

so a reliable ordered record one ahead of its turn is delivered at once instead of held. The
anchor occurs exactly once in that file at BASE. It mirrors the reversed witness
`tla/MCRNChannelBrokenOutOfOrder.cfg`. It must be found by `ordered_in_order` within N = 20 seeds,
`--shape push --seeds 1-20` (any loss or reorder on channel 3, 7 or 8 trips it; the expected finding is in
the first seed).

**The ack plant.** File `sdk/rocketnet_ack.cpp`, function `rocketnet_ack_endpoint_process_block`
(the sender's walk of a received ack block). Replace the one line

        newly_acked += rocketnet_ack_apply_run( endpoint, low, high, time,

(four spaces of indent: the first range's call; the call inside the loop below it has eight and is
not matched) with

        newly_acked += rocketnet_ack_apply_run( endpoint, low > 1 ? low - 1 : low, high, time,

so the sender treats the sequence just below each block's newest run as acknowledged. That
sequence is the top of a gap the receiver's block reported, so it was sent and never opened. The
later ranges are walked from the unchanged `low`, so only that one sequence is overclaimed, and
`low > 1` keeps the run's floor at sequence 1. The anchor occurs exactly once in that file at BASE.
The block itself is untouched and passes `rocketnet_ack_block_read`, so the overclaimed id reaches
the link's `acked()`. It is the runtime counterpart of the overclaim witness
`tla/MCFragmentsAndAcksWitOverclaim.cfg` (which in that model violates `BurnedNeverNamed`), and
breaks RNReceipts `AckedMeansPacketOpened` and RNChannelTypes `AckedImpliesTransportArrival`, which
`acked_means_opened` mirrors. It must be found by `acked_means_opened` within N = 20 seeds,
`--shape push --seeds 1-20`: the first lost packet below one the peer opened trips it. The message
layer then also stops resending what that packet carried, which `liveness_delivered` would catch
later; the selftest requires the earlier check.

**How it is shown.** `scripts/dst-selftest` (section 8) runs, per plant: copy the named file to
`build/plant/<name>/`, apply the replacement with a script that fails loudly unless the anchor
matches exactly once, configure a separate build directory with
`-DROCKETNET_DST_PLANT=<sdk file>=<copy>`, which builds `dst_planted` against `ROCKETNET_SOURCE` with
that one file swapped for the copy, run `dst_planted --shape push --seeds 1-20`, require exit 1 with a FAIL line
whose check is the plant's, then rerun that seed's printed replay command and require the
identical FAIL line. The seed that failed, for each plant, is written into the "Plant seeds"
line of section 6.

## 6. The budget

**Measured** at BASE `800119c`, Release (`-O2`, the bounded build), on two 64-core Linux fleet
members (the soak_sim and functional rows on one, the pair row on the other), each at a load near
or above 1 per core while measuring and the runs at nice 19, so these are pessimistic:

| What | Runs | Wall |
|---|---|---|
| `ROCKETNET_SOAK_SIM_SEED=1 ./bin/soak_sim 20000` (8 clients, 100 simulated seconds) | 3 | 1.80, 2.11, 2.15 s |
| `ROCKETNET_SOAK_SIM_SEED=1 ./bin/soak_sim 50000` | 3 | 5.40, 6.07, 6.12 s |
| `ROCKETNET_SOAK_SIM_SEED=1 ./bin/soak_sim 100000` | 3 | 9.24, 13.39, 17.65 s |
| `./bin/functional --run harness_one_message_each_way_lossy`, 200 seeds, one process each | 1 | 8.60 s (43 ms a process) |
| One message-layer pair (the channel harness, both directions sending one message a step over the eight types, 2% loss, 2% duplication, 50 ms latency, 20 ms jitter), 4000 steps of 10 ms, 20 seeds in one process, three times | 60 | 0.118-0.126 s a seed mean, 0.104 s best; 29.5-31.5 µs a step |

The pair figure comes from a throwaway program over `functional/channel_harness.h`, compiled
against the built `librocketnet_dev.a` and not committed; its delivered count was identical across
all three repeats of the 20 seeds (144,444), which is the in-process determinism the DST relies on.

soak_sim's summary line at seed 1 was identical across the three runs of each count (31, 73 and
140 connects; 0 failures), the determinism leg's claim. soak_sim costs about 90 µs a step for 8
clients at the protocol layer, about 11 µs per client-step.

**Estimated per seed** (an estimate from the pair figure; `rocketnet-dst-harness-core` measures
the real one and writes it beside this): one client-step of the DST costs about one pair-step, 30 µs,
since the pair's step is one client plus the server's share for that client, plus 30% for the
checks and the trace, about 40 µs.

| Shape | Mean clients | Steps | Estimate a seed |
|---|---|---|---|
| `push` | 3 | 2000 | 3 x 2000 x 40 µs = 0.24 s |
| `full` | 5 | 4000 | 5 x 4000 x 40 µs = 0.80 s |

A process per seed would add about 43 ms each (measured above), which is why every seed of a range
runs in one process.

**The per-push shard.** `ci.yml` gains a `dst` job with a matrix of 4 shards on the fleet's Linux
runner label (`[self-hosted, Linux, X64, fleet]`, as the other Linux jobs). Each shard builds only
the `dst` target, then `scripts/dst-sweep --shape push --seeds <a>-<b> --jobs 16` splits its 1,024
seeds into 16 processes of 64 seeds each, in parallel, never more processes than the runner's
cores. At the 0.24 s idle estimate a process runs 64 x 0.24 = 15 s, so an idle shard is the build plus
about 15 s, and the job is 4,096 seeds a push: shard k (0..3) runs seeds `k*1024+1` to `(k+1)*1024`. The
per-push seeds are fixed, so a push red is a regression at a known seed, never a lucky draw. The
shard's run leg is measured on a proxy: the message-layer pair program (above) run as 16 processes
in parallel on a 64-core Linux member under other load (load average 57), each 64 seeds of 6,000
pair-steps (3 clients x 2,000 steps), three repeats: 27.5, 27.6 and 29.5 s wall, a pair-step costing
66-69 µs under that contention against 30 µs idle. Adding the estimated 30% for the checks and the trace (an estimate, not a measurement), the run
leg is 36-38 s under that contention, which leaves a budget of 22-24 s of the 60 s for the build of
the `dst` target. The proxy's 3 clients x 2,000 steps is the `push` shape's mean; the load level is
the member's, not a measured CI runner's. The `dst` binary itself and that build are not measured here; `rocketnet-dst-ci-wiring`
measures each shard's wall time and records it in this section, and if a shard is over 60 s it
lowers the seeds per shard and raises the shard count (the job is split, never shortened), keeping
at least 4,096 seeds a push. The selftest (section 5) is a fifth job beside the shards, not a step
inside one. Its determinism and clean legs cost a build and two short runs; its two plant legs each
cost a second and third library build (the planted library recompiles `ROCKETNET_SOURCE` with one
file swapped) plus 20 seeds and a replay, about 5 s of runs each at the estimate. The builds are not
measured here: if the selftest job is over 60 s, the ci-wiring card moves the two plant legs to
`nightly-dst.yml` (they then run every night, before the campaign) and keeps the determinism and
clean legs per push; it records which in this section.

**The nightly campaign.** `.github/workflows/nightly-dst.yml`, on schedule and on
`workflow_dispatch`, with refs taken as `nightly-channel-seeds.yml` takes them, runs 1,000,000
`push`-shape seeds and 20,000 `full`-shape seeds each night, on a seed range that moves: the night's
first seed is `1,000,000 * (days since 2026-01-01) + 100,000` for `push` (clear of the per-push
range: the per-push seeds are 1..4,096) and `10,000,000,000 + 20,000 * (days since 2026-01-01)` for
`full`, so no night repeats another and no two ranges overlap. It is split into 8 matrix jobs
of 125,000 `push` seeds and 2,500 `full` seeds each: at the estimates that is 125,000 x 0.24 +
2,500 x 0.80 = 32,000 core-seconds a job, 33 minutes on a 16-core runner and 17 on a 32-core one,
under a 90-minute timeout. The total is about 256,000 core-seconds (71 core-hours) a night,
estimated; the ci-wiring card records the measured figure.

**Plant seeds.** The seed at which each plant of section 5 failed, as `scripts/dst-selftest`
measures it: ordering, not yet run; ack, not yet run (written by `rocketnet-dst-ci-wiring`).

**Failure capture and minimisation.** The nightly prints every `FAIL seed=<n> ...` line with its
replay command and uploads the log on failure. For each failing seed (at most 20), `scripts/dst-sweep
--minimise <seed>` reruns it with each fault kind switched off in turn (`--faults`), keeping a kind
off when the same check still fails at that seed; then with `--clients` lowered one at a time while
it still fails. Because each kind draws from its own substream, the client count is drawn before it
is overridden, and every per-client draw is the client's own (section 2), switching a kind off or
removing a client leaves every other draw identical, so the reduction is of one failure, not a new
one. `--steps` is not a reduction: it moves every fault time, so the minimiser never changes it. It prints the smallest replay command found. A minimised seed that fails is a defect card with
that command as its RED AT BASE.

## 7. The state machines

**The fault injector (crash, restart and skew).** Each peer is up, crashed or restarting; each
client carries a session generation; each peer carries a clock offset and rate. That is a state
machine, so it has a model before its code: `tla/RNSimFaults.tla` (card
`rocketnet-dst-fault-model`). Variables: each peer's phase (up, crashed, restarting), each client's
session generation and the set of generations ever used, each peer's clock offset, rate and last
time, the true time, the fault window flag, and the in-flight packets tagged with their generation.
Actions: `Crash(p)`, `Restart(p)`, `Tick` (time advances, each clock moves by its rate), `EndFaults`,
and the outside events `Connect(c)`, `Disconnect(c)` and `Deliver`. Invariants:
`CrashedPeerSilent` (a crashed peer sends and receives nothing), `RestartIsFresh` (a restarted
client's generation is fresh, never an earlier one), `DeliverOnlyToLiveGeneration` (no delivery
reaches a generation other than the live one), `SkewBounded` and `ClockMonotone` (every clock stays
inside the bound of section 3 and never runs backwards). Liveness under weak fairness once
`EndFaults` has run: `EveryLiveClientConnects`. Cases: `MCRNSimFaults.cfg` passes; the reversed
witnesses `MCRNSimFaultsBrokenRestartKeepsSession.cfg` (restart keeps the old generation; violates
`RestartIsFresh` and `DeliverOnlyToLiveGeneration`) and `MCRNSimFaultsBrokenCrashedSends.cfg` (a
crashed peer still delivers; violates `CrashedPeerSilent`) and
`MCRNSimFaultsBrokenSkewBackwards.cfg` (a rate that can go negative; violates `ClockMonotone`) each
fail on the property named. The model card adds the `tla/COVERAGE.tsv` row (machine
`dst-fault-injector`, module RNSimFaults, status owed, owed_by `rocketnet-dst-fault-injectors`); the
injector's code (`dst_faults.h`) cites each action and invariant it implements, and that card moves
the row from owed to covered.

**The seed run.** Fault window, quiet window, final checks: a fixed sequence of three phases with no
branching and no outside event; it is not a state machine in the sense the models cover, and gets no
module. The minimiser is a loop over a fixed list of reductions, likewise.

**The seam.** The N-client `rn_message_link_t` binding has no state of its own beyond the per-slot
connection id it reports, which follows the server's slot state; that state is RNSlots'
(`tla/RNSlots.tla`) and ServerSession's, already modelled. The checks of section 4 mirror those
models rather than adding one.

## 8. The cards

Five cards build the DST: one heavy model card and four pro cards, in dependency order. Each
owns the files listed. Two files are shared, by rule: `dst_harness.h` is written by harness-core,
which declares every hook the later cards fill (below), so the injector and check cards, which run
in parallel, each write only their own header and touch `dst_harness.h` only to fix a defect in it;
and `docs/DST.md`, where each card edits only its own section (harness-core and ci-wiring section 6,
the model card section 7, the injectors section 3, the checks section 4). A file not listed is
touched by no DST card.

**`rocketnet-dst-harness-core`** (pro; needs this design). The world, the seam, the run and its
output. Owns: `dst_harness.h` (new: the world lifted from soak_sim, the N-client
`rn_message_link_t` seam, substreams, the payload scheme and the sender's per-stream record, the
trace hash, the `keys` randombytes implementation, the run loop with `--faults none` behaviour, and
the hook declarations: `dst_faults_step`, `dst_faults_send_time`, `dst_faults_clock` and
`dst_faults_peer_live` for the injectors, and `dst_check_delivery`, `dst_check_receipt`,
`dst_check_acked`, `dst_check_opened`, `dst_check_step` and `dst_check_end` for the checks, each
defined as a no-op until its card lands, with the arguments sections 3 and 4 name), `dst.cpp` (new:
the command line of section 2, every flag parsed, including every `--faults` name),
`test_unit_dst_core.cpp` (new), `soak_sim.cpp` (calls the lifted world; output byte-equal),
`CMakeLists.txt` (the `dst` target, and the `ROCKETNET_DST_PLANT=<sdk file>=<copy>` option that adds
a `dst_planted` target built against `ROCKETNET_SOURCE` with that one file swapped, section 5),
`docs/DST.md` (section 6's measured per-seed cost). The card's PATHS already carry these;
`scripts/soak-sim-selftest` is in its PATHS but is not changed by this design.

**`rocketnet-dst-fault-model`** (heavy; needs this design). Section 7. Owns `tla/RNSimFaults.tla`,
`tla/MCRNSimFaults.tla`, `tla/MCRNSimFaults.cfg`, `tla/MCRNSimFaultsBrokenRestartKeepsSession.cfg`,
`tla/MCRNSimFaultsBrokenCrashedSends.cfg` and `tla/MCRNSimFaultsBrokenSkewBackwards.cfg` (new),
`tla/CASES.tsv`, `tla/RUNS.tsv` (by `tlacheck` only), `tla/COVERAGE.tsv`, `tla/README.md`,
`docs/TLA-INVENTORY.md` and `docs/DST.md`. The third witness is not in the card's NEW today: PATHS-PROPOSED.

**`rocketnet-dst-fault-injectors`** (pro; needs harness-core and fault-model). Section 3. Owns:
`dst_faults.h` (new: the four fault hooks' definitions), `test_unit_dst_faults.cpp` (new),
`tla/COVERAGE.tsv` (the RNSimFaults row, owed to covered), `docs/DST.md`. It needs neither
`sdk/rocketnet_network_simulator.h` nor `.cpp`, because reorder and delay are made by the send time
(section 3); both stay in its PATHS only for a defect found there, as do `soak_sim.cpp`, `dst.cpp`,
`dst_harness.h` and `CMakeLists.txt`.

**`rocketnet-dst-invariant-checks`** (pro; needs harness-core). Section 4. Owns: `dst_checks.h`
(new: the six check hooks' definitions and every check of the table), `test_unit_dst_checks.cpp`
(new), `tla/COVERAGE.tsv` (a row per mirrored invariant), `docs/DST.md`. The checks that read
fault state (`crashed_silent`, `skew_bounded`, `fresh_generation`, the explained terminal windows)
read it through `dst_faults_peer_live` and `dst_faults_clock` and the fault events in the trace, and
pass trivially until the injectors land. When both land, the second to land rebases on the first;
their files do not overlap.

**`rocketnet-dst-ci-wiring`** (pro; needs the injectors and the checks). Sections 5 and 6. Owns:
`.github/workflows/ci.yml` (the dst job and the selftest job), `.github/workflows/nightly-dst.yml`
(new), `scripts/dst-selftest` (new), `scripts/dst-sweep` (new: the shard and nightly runner, and
`--minimise`), `docs/DST.md`. The planted build uses the `ROCKETNET_DST_PLANT` option harness-core
adds, so this card needs no `CMakeLists.txt` edit.

Later, outside these five: rebinding `functional/channel_harness.h` to the DST's seam, and
switching the payload scheme to `test/live_verify.h` once that header is on BASE, each a small card
of its own when wanted.
