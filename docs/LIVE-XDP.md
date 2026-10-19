# The live XDP fleet run: a stress test of the internal protocol

This document is the design of one live run. One server runs on the
dedicated test server with the xdp driver. One client runs on each fleet
bench, all at the same time. The run drives every message type and every
channel, crosses every size boundary, cycles reconnects, and verifies every
message in both directions. It is a design: nothing here is built yet. Three
cards build and run it (section 14).

Every decision names the file and line it was read from. Line numbers are
at `sprint/rocketnet-proof` `20d8b35`; `client.c`, `server.c`, `soak.c`,
`server_xdp/server_xdp.c` and `sdk/rocketnet.h` are byte-identical at
`8ddc93ec1771`, so the same lines hold there. The owner's rulings of
2026-10-18 sit on top of the card that asked for this document. Where the two
differ, the rulings win, and section 15 lists each difference. The base tip
this design lands on, `962e3a4`, has since raised the `max_clients` default
to 1,000 (`rocketnet-max-clients-defaults-to-1000`, the same line 923) and
added a Windows server driver row to `sdk/rocketnet_drivers.inc`; no other
cited line moves.

The run's lifecycle is modelled in `tla/RNLiveFleetRun.tla` (section 13).

## 1. Framing

The run tests rocketnet's internal protocol, not game behaviour. That means
the message records and their forms, channels, the eight reliabilities,
blocks and their chunks, the transport's fragments, reliable resend, the ack
and message windows, the send budget, the slot table, the handshake, and the
timeout and disconnect paths. The library's functional suite runs these over
loopback and the simulator (`functional/channel_harness.h:5-17`). This run
puts them on real NICs, a real 10 Gbit XDP data plane, real LAN and internet
paths, and about ten peers at once.

- There is one client per bench, and all of them run at the same time.
- The server is the only process on the test server's side.
- Every payload message is verified in both directions, in every mode and on
  every channel at once (sections 3 to 5).
- Any failed connect, any denial and any unexpected disconnect fails the run
  (section 7).

The run follows the soak discipline of `soak.c:5-9`: duration turns
probability into certainty. It also keeps soak's counter discipline. Every
counter family must balance (`soak.c:54`, `rocketnet_counters_conserved`).
Every refusal counter must read 0, except the ones section 7 names, each
with its mechanism and an exact expected count.

## 2. Programs

The run uses the repository's example programs, extended. It adds no
separate harness.

**Server: `server.c`, with the driver pinned to xdp.** `server_xdp/server_xdp.c`
is the kernel XDP program, not a server. The library embeds and loads it
itself (`CMakeLists.txt:75-89`; `sdk/rocketnet_server_socket_linux.cpp:1302-1352`).
It is read-only for every card of this run. The xdp driver is a runtime
choice. The config's `driver` field (`sdk/rocketnet.h:933`) or
`ROCKETNET_SERVER_DRIVER` gives the candidate list, and a one-element list is
a pin: that driver or a loud failure, never a silent fall to gso or portable
(`sdk/rocketnet_server_transport.cpp:54-67`; registry row
`sdk/rocketnet_drivers.inc:7`). The run starts `server` with
`ROCKETNET_SERVER_DRIVER=xdp`. It also passes `--driver xdp`, so the config
records the pin. It reads `rocketnet_server_transport_driver`
(`sdk/rocketnet.h:1062`) after create and refuses to continue unless it
reads `xdp`.

**Client: `client.c`.** `soak.c` stays the loopback soak. It owns both ends
in one process (`soak.c:115`, `soak.c:164`) and cannot be split across
machines.

Today both programs exchange transport payloads, not messages: `client.c`
calls `rocketnet_client_transport_send_packet` (`client.c:91`), and
`server.c` echoes payloads back (`server.c:76-82`). Each gains a `--matrix`
mode built on the message objects (`rocketnet_message_client_create`,
`rocketnet_message_server_create`, `sdk/rocketnet.h:1756-1878`). Without
`--matrix`, each program behaves exactly as it does today; it is the
out-of-the-box example. The new flags, on both programs unless marked:

| Flag | Meaning |
|---|---|
| `--matrix` | Run the message matrix instead of the payload echo. |
| `--address A:P` | Server: the public address it binds and that tokens name. Client: the address it mints tokens for. Positional `argv[1]` keeps working (`client.c:34`, `server.c:38`). |
| `--label L` | The bench label written on every result line. It comes from the inventory (section 8), never from a hostname. |
| `--duration S` | Client: the stress window length in seconds (section 6). |
| `--cycles N` | Client: reconnects before the final window (section 6). |
| `--stagger S` | Client: seconds to wait before the first connect (section 6). |
| `--seed X` | The run seed, printed at start. It offsets the length rule (section 4). Both ends must be given the same seed. |
| `--modes M` / `--channels C` | The modes and channel count to drive. Defaults: all eight modes and the config's `num_channels` (32, `sdk/rocketnet.h:1533`). |
| `--send-kbps K` | The send budget: the client's own, or the server's per client (`sdk/rocketnet.h:1809`, `:1879`). |
| `--max-packets-per-update N` | The message config's `max_packets_per_update` (`sdk/rocketnet.h:1540`). It is not a WIRE field, so the two ends may differ. |
| `--max-clients N` | Server: `max_clients`, set explicitly (section 7). |
| `--update-hz H` | Update rate of the main loop: server through `rocketnet_frame_sync_create` (`sdk/rocketnet.h:1334`), client through `rocketnet_sleep`. |
| `--counters-every S` | Snapshot every counter, RSS and the transport stats every S seconds (default 60), and at start and exit. |
| `--results PATH` | Where the result lines go (section 7). |
| `--status PATH` | Server: one line per state change (`listening`, `stopping`, `stopped`), for the orchestrator. |

**Linking.** Both programs already link `rocketnet_dev` (`CMakeLists.txt:275-278`).
That library is compiled with `ROCKETNET_DEVELOPMENT=1` (`CMakeLists.txt:159-163`,
`:220-221`), and only that build carries `rocketnet_write_dev_connect_token`
(`sdk/rocketnet_connect_token.h:186-191`) and the published development keys
`rocketnet_server_transport_dev_config` fills (`server.c:41`). The run uses
those development keys. The run is a protocol test on a dedicated machine
with its own filter value, not a deployment.

**The shared header.** `test/live_matrix.h` holds the verify scheme, the
length schedule, the per-stream receive state and the send-pacing ledger. It
is plain C99 `static inline`, with no allocation and no syscall. `client.c`
(a C file), `server.c` and the functional test (section 14) include it, so
the generator and the checker are one piece of code on every platform.

## 3. Verify scheme

**The source.** The owner's sequence-derived pair is `generate_packet_data`
and `check_packet_data` in mas-bandwidth/reliable `soak.c:82-134` at
`ea82613c5560`; the card quotes it. The rule:

- The length comes from the sequence: `((sequence * 1023) % (MAX - 2)) + 2`.
- Bytes 0 and 1 carry the sequence.
- Byte `i` is `(i + sequence) % 256`.

The checker recomputes the length from the sequence it reads, so a message
with the right bytes but the wrong length fails. rocketnet's own pair
(`soak.c:26-41`, `sdk/rocketnet_tests.cpp:2306-2322`) derives the contents
from the length alone. A lost, duplicated or reordered message of the same
length therefore passes it. The run needs the sequence form.

**Kept, not replaced.** The length-only pair stays where it is. It verifies
transport payloads in `soak.c` and in the unit suite, and nothing in this
run changes those. The new pair is for messages only.

**The message header.** Every message of 7 bytes or more begins with:

| Bytes | Field | Why |
|---|---|---|
| 0 | tag: `0xA0 | direction << 3 | mode` (mode 0 to 7, direction 0 = client to server) | The receive call alone cannot name the mode. Codes 0 and 2 carry no channel on the wire (`sdk/rocketnet_message.h:112-116`), and 5 and 6 travel as 0 and 2 (`sdk/rocketnet_message.h:31-33`). The constant top nibble catches garbage. |
| 1 | the channel the sender passed (0 to 255) | Checked against the channel the message was received on, by the rule of section 5. |
| 2 | the connection's cycle number, mod 256 | A message from an earlier connection fails on its first byte past the channel. This is the "no leak from the old connection" check of section 6. |
| 3 to 6 | the stream sequence, u32 little endian | One counter per (direction, mode, channel) stream, starting at 0 on every connection. |
| 7 on | byte `i` = `(i + sequence) % 256` | The owner's byte rule. `i` counts from the start of the message, as in the source. |

**Why 32 bits.** The fastest stream sends under 10^6 messages a second, so a
35 s window uses under 2^26 sequences. A u32 never wraps in a window. The
source's u16 would wrap in under a second at LAN rates, and then a message
65,536 behind would verify. Sequences restart on every connection, so the
width only has to cover one window.

**Messages under 7 bytes** carry the first L bytes of the header and nothing
else. A 1-byte message proves its tag, a 2-byte one its tag and channel, and
a 3-byte one its tag, channel and cycle; none of the three carries its
sequence. Section 4 places these lengths only at fixed sequence
positions. On RELIABLE_ORDERED and its receipt form, the expected next
sequence is known exactly, so the checker checks it. On the other reliable
modes, completeness is checked by count: the receiver counts the messages of
each tiny length, and the count must equal the number of sent sequences that
map to that length. On the unreliable modes, a 1 to 3 byte message proves its
mode, channel and connection only. This is the scheme's one blind spot, and
it is stated here, not hidden.

**What the checker records.** It checks every received message: its length
against the length rule for the sequence it carries, and every byte. Each
mismatch, short message, bad length, wrong tag, wrong channel, wrong cycle
and duplicate is one failure line:
`fail <label> <cycle> <direction> <mode> <channel> <sequence> <reason> <bytes>`.

## 4. Sizes

**The modes' maxima.** Each value is the largest message a mode accepts at
the run's config (message config defaults, client `max_payload_bytes`
default 16,384 at `sdk/rocketnet.h:560`):

| Mode | Maximum `max_m` | Source |
|---|---|---|
| 0 UNRELIABLE | 1,411 | `unreliable_limit` = 1415 - 1 - 3 (`sdk/rocketnet_endpoint.cpp:47`) |
| 1 UNRELIABLE_SEQUENCED | 16,373 on channels below 256 | the smallest of 65,535, `max_message_bytes` and the transport cap less the 10-byte header and the form byte (`sdk/rocketnet_endpoint.cpp:150-157`) |
| 2, 3, 4, 6, 7 (reliable) | 262,144 | `max_message_bytes` (`sdk/rocketnet.h:1536`; `sdk/rocketnet_channel.cpp:814`) |
| 5 UNRELIABLE_WITH_ACK_RECEIPT | 262,144 | above 1,411 it goes as a reliable code-2 block (`sdk/rocketnet_endpoint.cpp:158-176`; `docs/CHANNELS.md` 7.4) |

**The boundary list B.** Each value is clipped to the mode's maximum and
skipped when it is not a valid length (below 1):

- **The header prefix:** 1, 2, 3, 4, 5, 6, 7.
- **The single-record edge E_m, at -1, 0 and +1.** E is 1,411 for codes 0
  and 5 (`sdk/rocketnet_endpoint.cpp:47`) and 1,401 for codes 3 and 7 (the
  payload capacity less 14, `docs/CHANNELS.md` 8 "As built";
  `sdk/rocketnet_channel.h:117`). It is derived as 1,408 for code 2 and 1,399
  for code 4 from their header widths (`sdk/rocketnet_message.h:52-55`). The
  matrix functional test's first case asserts each derived E against the
  library by sending E and E + 1. A difference is a finding.
- **The transport payload edges, for code 1,** whose message rides one
  RECORDS payload with an 11-byte overhead (the 10-byte header and the form
  byte, `sdk/rocketnet_endpoint.cpp:153-157`): message lengths 1,404, 1,405
  and 1,406, where the payload crosses 1,415 and 1,416
  (`ROCKETNET_MESSAGE_SINGLE_BYTES` and `ROCKETNET_MAX_PAYLOAD_BYTES`,
  `sdk/rocketnet.h:1503`, `:543`). The transport fragment stride is 1,413
  (`sdk/rocketnet_fragment.h:37`), so the list adds the message lengths
  k x 1,413 - 11 - 1, 0 and +1 for k = 2, 3 (the payload filling exactly k
  fragments), and the payload maximum of 16,384 less the 11 bytes with its
  neighbours.
- **The block stride and the store's chunk:** F = 1,152
  (`sdk/rocketnet_message.h:66`). That is also the send store's large class
  (`sdk/rocketnet_allocator.h:233`), with spans from F + 1
  (`sdk/rocketnet_allocator.h:410`). The list adds k x F - 1, k x F and
  k x F + 1 for k = 1, 2, 3.
- **The near-maximum values:** 16,383, 16,384 and 16,385 (the default
  transport payload maximum, `sdk/rocketnet.h:560`), `max_m - F - 1`,
  `max_m - F`, `max_m - 1` and `max_m` exactly.

**The over-maximum send.** `max_m + 1` is never sent. Once per window, per
direction and per mode, on channel 0 and on the last channel, the runner
calls start with `max_m + 1`. Start must return NULL, the connection must
stay up, and the named counter must rise by exactly one:

- `MESSAGE_SEND_REFUSED` for the reliable path (`sdk/rocketnet_channel.cpp:814`,
  counted at `sdk/rocketnet_endpoint.cpp:136-141`).
- `MESSAGE_UNRELIABLE_SEND_FAILED` for codes 0 and 1
  (`sdk/rocketnet_endpoint.cpp:158-164`).

The runner logs `refused <mode> <bytes> <counter> <delta>`. A start that
succeeds, a different counter, or a delta other than 1 is a failure.

**The length rule.** Within a stream (direction, mode, channel), sequence s
takes a slot `r = (s + channel) % 32`:

- `r = 0`: the boundary slot. The length is `B[(s / 32 + channel x ceil(|B| / 32)) % |B|]`.
  The channel offset spreads the list across the 32 channels. Each
  (direction, mode) then walks all of B once every channel has passed about
  `32 x ceil(|B| / 32)` sequences, so a window covers B even on the internet
  paths.
- `r = 1 to 23`: small, from 7 to E_m.
- `r = 24 to 29`: medium, from E_m + 1 to min(16,384, max_m).
- `r = 30 to 31`: large, from 16,385 to max_m (small instead, where max_m is
  below 16,385).

Inside a bucket from lo to hi, the length is
`lo + (((s + seed) * 1023) % (hi - lo + 1))`, computed in 64-bit unsigned
arithmetic (s x 1023 passes 2^32 near s = 2^22). This is the source's rule over
the bucket's range, with the run seed added. The receiver recomputes the
length exactly from s, the channel, the seed and the mode.

**Size buckets, as named in the results:** `tiny` (1-6), `small`, `edge`
(E_m and its neighbours), `fragment` (every k x F and k x 1,413 neighbour),
`medium`, `large`, `max` (exactly max_m) and `over` (max_m + 1, refused).

## 5. Types and channels

**Every mode.** The public header numbers eight reliabilities
(`sdk/rocketnet.h:1460-1467`). At BASE all eight are enabled: the channels
take the first-version mask when given 0 (`sdk/rocketnet_channel.cpp:189`),
and that mask sets all eight bits (`sdk/rocketnet_channel.h:59`). The
ordered path passes 0 (`sdk/rocketnet_channel.cpp:1023`). Two comments
disagree with the code. The object comment lists five enabled types
(`sdk/rocketnet.h:1708-1716`), and `docs/CHANNELS.md` 7.5 says the mask is
{0, 3}. The run drives all eight and counts `MESSAGE_SEND_TYPE_NOT_ENABLED`,
which must stay 0. Section 15 lists the stale comments.

**The block form** applies to every reliable mode above its single-record
edge, and to mode 5 above 1,411 (`docs/CHANNELS.md` 8; 7.4).

**Every channel, all at once.** The config allows 32 channels
(`sdk/rocketnet.h:1533`). Every stream (direction x mode x channel) runs in
every window. The channel-less modes 0, 2, 5 and 6 name all 32 channels; the
modes 1, 3, 4 and 7 run on channels 0 to 30, because channel 31 carries the
progress reports and the HELLO (below and section 6). That is
2 x (4 x 32 + 4 x 31) = 504 streams per connection. Each update, the
receiver polls every channel with `rocketnet_message_*_receive`, and it
releases every view before the next update (`sdk/rocketnet.h:1726-1731`).
The receive channel is checked against the sender's channel byte. Codes 1,
3, 4 and 7 must arrive on the channel sent. Codes 0, 2, 5 and 6 arrive on
channel 0, because their records carry no channel (`sdk/rocketnet.h:1716`;
`sdk/rocketnet_message.h:112-116`). The functional test pins this rule per
code first. A disagreement is a finding, not a pass.

**The receive checks, per mode:**

| Mode | Every arrival | Completeness and order |
|---|---|---|
| 0 UNRELIABLE | verifies | loss counted: sent - received, per stream per window. No order is promised. |
| 1 UNRELIABLE_SEQUENCED | verifies | sequences strictly increase. Skips count as superseded (`MESSAGE_SEQUENCED_SUPERSEDED`, `sdk/rocketnet_counters.h:485`) or lost. |
| 2 RELIABLE | verifies | every sequence sent in the window arrives exactly once, in any order. |
| 3 RELIABLE_ORDERED | verifies | every sequence arrives exactly once, in order: each arrival equals the expected next. |
| 4 RELIABLE_SEQUENCED | verifies | strictly increasing. Each channel's drain closes with one mode-4 message sent after that channel's last ordered send, and that message must arrive. |
| 5 UNRELIABLE_WITH_ACK_RECEIPT | verifies | as mode 0 at 1,411 bytes or less, as mode 2 above. Every serial gets exactly one receipt (`docs/CHANNELS.md` 7.4). |
| 6 RELIABLE_WITH_ACK_RECEIPT | verifies | as mode 2, plus exactly one ACKED receipt per serial and no LOSS. |
| 7 RELIABLE_ORDERED_WITH_ACK_RECEIPT | verifies | as mode 3, plus exactly one ACKED receipt per serial and no LOSS. |

In every mode, a duplicate is a failure. The receiver keeps a 65,536-bit
seen-window per stream for the unordered modes. Receipts are read every
update (`sdk/rocketnet.h:1803`), so `MESSAGE_RECEIPT_RING_FULL` stays 0.

**The drain.** A window does not end mid-flight. The sender stops new sends,
then waits until the peer's progress reports show that every reliable
sequence has arrived. Only then does it disconnect. The wait is bounded by
`reliable_timeout_ms` (10 s, `sdk/rocketnet.h:1547`). This is how
completeness is defined even on a silent-drop window: the drop comes after
the drain.

**Sequenced and ordered share a channel's epoch.** An ordered send resets
the channel's sequenced counter (`sdk/rocketnet_channel.cpp:240-252`), and a
sequenced message from an older epoch is not delivered after a newer ordered
one (`sdk/rocketnet_channel.cpp:288-298`). Running modes 1 and 4 on the same
channels as 3 and 7 tests that coupling, so `MESSAGE_SEQUENCED_STALE` moves
by design as well as by loss. That is why the closing mode-4 message above
follows the channel's last ordered send.

**Progress reports.** Every 50 ms each end sends one UNRELIABLE_SEQUENCED
message on the last channel (31) with tag nibble `0xC`. It carries, per
stream, the highest contiguous sequence received and the bytes received. It
is verified like any message. The sender's pacing ledger and the drain read
it. Channel 31 carries no other mode-1 or mode-4 stream, and no ordered
stream, so the reports never supersede, or are superseded by, a matrix
stream: the matrix's modes 1, 3, 4 and 7 run on channels 0 to 30, and the
channel-less modes 0, 2, 5 and 6 name all 32.

## 6. Load and the cycle

**Duration.** Each client runs 9 windows: 8 reconnect cycles, then a final
window. A window is 35 s of stress plus a drain, normally under 1 s and at
most 10 s (`reliable_timeout_ms`). Between windows the client waits 2 x the
timeout, which is 20 s at the run's token (below). One cycle is about 56 s.
One client's run is about 9 x 36 + 8 x 20 = 484 s, and at worst, with every
drain at its 10 s bound, 9 x 45 + 8 x 20 = 565 s. The stagger adds at most
one cycle, 56 s, so the run is about 9 minutes and at worst 10.4 minutes,
the owner's example. Load and listen take under 30 s and teardown under 60 s,
so the XDP-attached time is at worst about 12 minutes, inside the dead-man
timer's 15 (section 9). The orchestrator computes the attached time from the
worst-case drains and refuses a plan over 13 minutes.

**The timeout, read, never a literal.** The connection timeout is the
token's `timeout_seconds`. The development token writes it as a literal 10
(`sdk/rocketnet_connect_token.cpp:285`), and the server caps it at
`max_timeout_seconds`, 60 (`sdk/rocketnet.h:935`). The need
`rocketnet-dev-token-timeout-is-a-named-constant` adds a named constant,
`ROCKETNET_DEV_CONNECT_TOKEN_TIMEOUT_SECONDS`, beside
`ROCKETNET_DEV_CONNECT_TOKEN_EXPIRE_SECONDS` (`sdk/rocketnet_connect_token.h:171`);
the runner card does not.
Both programs read the wait from that constant. The client prints the value
it waits on, and the merge refuses a run whose two ends printed different
values.

**Rates: the cap is a total, set by the site.** The send budget is what
pushes the protocol. It is configuration, not a compile-time limit: the
message config fields `client_send_kbps` and `server_send_kbps` carry the
defaults, 1,000 kbps for a client and 10,000 kbps per client for the server,
and each runner sets its own caps explicitly in its config. Under RULINGS
(b) and (d) every client's cap is its total, both directions combined, and
the site sets it:

| Path | Client send | Server per-client send | `max_packets_per_update` | Update rate |
|---|---|---|---|---|
| Studio site (1 Mbit/s total) | 500 kbps | 500 kbps | 64 | 1,000 Hz |
| Datacenter (100 Mbit/s total) | 50,000 kbps | 50,000 kbps | 64 | 1,000 Hz |

No client runs the LAN flat-out rate against the test server; anything
heavier belongs in the LAN phases (`docs/LAN-BENCH.md`). The studio site
reaches the test server over one shared internet uplink of 20 to 50 Mbit/s,
so every bench on it, including both sides of the Linux-on-Windows bench and
the macOS bench, is capped at 1 Mbit/s total; the datacenter benches do not
cross that uplink and keep 100 Mbit/s total.

**The server's per-client cap.** The server takes one budget at create
(`sdk/rocketnet.h:1823-1826`), so it cannot know a client's site from its
flags alone. Each client's first message on every connection is a HELLO: one
RELIABLE_ORDERED message on channel 31, tag nibble `0xD`, carrying the bench
label and the path's cap in kbps from the inventory. Under RULINGS (d) the
server creates with the lowest cap, 500 kbps, and on HELLO calls
`rocketnet_message_server_set_send_kbps( server, client_index, cap )`
(`sdk/rocketnet.h:1879`), raising the slot only to that slot's inventory cap.
So no studio client is ever sent more than its cap, even before its HELLO
arrives. The server logs the label and cap per slot, which is also how the
results name the bench for each slot.

**The budget.** Under RULINGS (d) the run is limited to 20 GB in total, of
which at most 1 GB crosses the studio uplink. The cap is a total for each
client, both directions combined. The send budget bounds wire bytes by
construction: no end sends more than its kbps x the time
(`sdk/rocketnet.h:1733-1736`). The plan's upper bound is therefore
`sum(client total kbps) x worst-case run seconds / 8`. At 621 s, seven
studio clients at 1 Mbit/s bound at about 0.54 GB and two datacenter clients
at 100 Mbit/s at about 15.5 GB, about 16.07 GB in all. The orchestrator
computes the run bound and the studio-uplink bound from the inventory before
it starts and refuses a plan over 20 GB or over 1 GB; the run records the
studio-uplink total separately. The library has no cumulative wire-byte
counter: the stats give trailing per-second rates only
(`sdk/rocketnet.h:830-839`, `:1284-1292`). So the reported actual total is
the integral of the per-second samples, labelled approximate, beside the
exact bound (section 15).

**The mix.** The generator offers 110% of the budget, so the budget always
binds and the send queues stay near their bounds. The offered bytes split by
mode: 0: 15%, 1: 15%, 2: 15%, 3: 20%, 4: 10%, 5: 5%, 6: 10%, 7: 10%.
Unreliable bytes over the budget are dropped and counted
(`MESSAGE_UNRELIABLE_OVER_BUDGET`, `sdk/rocketnet_counters.h:482`), which is
expected. Reliable messages wait for the budget and are never dropped
(`sdk/rocketnet.h:1722-1724`).

**Pacing, the backpressure the API does not give.** A start the region has
no room for ends the connection with `MESSAGE_MEMORY_EXHAUSTED`. It is not a
soft refusal (`sdk/rocketnet.h:1719-1724`; `docs/CHANNELS.md` 4, "Exhaustion
kicks"; `sdk/rocketnet_endpoint.cpp:134-141`). The API has no query for
free room, so the runner keeps its own ledger. It admits a reliable send only
while all three of these hold:

- outstanding bytes + L <= `send_buffer_bytes` / 2 (400,000 at
  `sdk/rocketnet.h:1537`);
- outstanding messages < `send_queue_messages` / 2 (512);
- outstanding reliable numbers + ceil(L / F) <= W / 2 (512).

Outstanding = sent minus what the peer's progress reports have confirmed.
The half margins cover the acks in flight. A send held back by the ledger
counts as `paced` in the runner's own counter. That is the run's
backpressure figure, recorded per mode. `MESSAGE_SEND_STORE_FULL`,
`MESSAGE_SEND_QUEUE_FULL` and `MESSAGE_MEMORY_EXHAUSTED`
(`sdk/rocketnet_counters.h:420-421`, `:469`) must stay 0. Any of them is the
kick, which is an unexpected disconnect and a FAIL. On the 1 Mbit/s studio
path the reliable modes are window-bound and the unreliable ones
budget-bound: about 400 KB per round trip against 62.5 KB/s each way. On a
100 Mbit/s datacenter path the same ledger faces 6.25 MB/s each way.

**Loss, reorder and resends.** The LAN loses little, and the internet path
loses and reorders some. Resends (`MESSAGE_RESENT`,
`sdk/rocketnet_counters.h:460`) and abandoned transport fragments
(`FRAGMENT_PAYLOAD_ABANDONED`, `:350`) are expected on the internet path and
near 0 on the LAN. The run injects no loss: the paths are the test.

**The ack and message windows wrap.** In every window, in each direction:

- **The message window W = 1,024** (`sdk/rocketnet.h:1534`): at least
  10 x W reliable numbers. The runner counts the numbers it uses exactly, as
  1 per small message and ceil(L / F) per block, and the pass table requires
  10 W.
- **The transport's sent-history ring of 4,096 packets**
  (`sdk/rocketnet_history.h:8`) and the 2,048-entry receive buffer
  (`sdk/rocketnet_constants.h:122`): at least 4 full wraps each. No counter
  gives packets sent, so each end counts its own: the message objects hand
  every packet through the transport's start and finish
  (`sdk/rocketnet_message_server.cpp:550-580`), and the runner reads the
  per-second rate from the stats each second and integrates it. The figure
  is labelled approximate, and the pass row uses a margin of 2: at least
  8 x 4,096 packets estimated per window per direction.
- **The 24-bit reliable number** (`sdk/rocketnet_message.h:68-70`) does not
  wrap. That takes 16.7 million numbers on one connection, and no window
  inside the 15-minute dead-man timer gets there. Its wrap is covered by
  `ChannelsWindowReceiveReconstructsAcrossThe24BitWrap` and `RNWindow`
  (`docs/CHANNELS.md:1968`). This run does not claim it.

**The cycle.** For each client, the run repeats:

1. Mint a fresh token (section 11).
2. Connect.
3. Run a stress window of the full matrix.
4. Drain.
5. Disconnect.
6. Wait 2 x the timeout.
7. Repeat.

The disconnect kind alternates, starting with clean:

- **Clean:** `rocketnet_client_transport_disconnect` on the message client's
  transport (`sdk/rocketnet.h:814`, `:1774`). Its redundant disconnect burst
  frees the server's slot at once. If the whole burst is lost, the timeout
  frees it.
- **Silent drop:** destroy the message client without a disconnect call.
  Destroy hands off the payloads already committed and sends no disconnect
  packet (`sdk/rocketnet_client_transport.cpp:705`, `rocketnet_client_transport_destroy`).
  The server's timeout path frees the slot (`CLIENT_TIMED_OUT`,
  `sdk/rocketnet_counters.h:190`). The merge checks the server's event log:
  every silent drop is followed by a timeout of that slot, every clean
  disconnect by a disconnect of that slot (or a timeout, when the whole burst
  was lost, which the log names), and `CLIENT_TIMED_OUT` plus
  `CLIENT_DISCONNECTED` equal the window ends.

After 2 x timeout, the slot is free either way. The run ends with a clean
disconnect.

**The stagger.** Sort the clients by label, with index i of N. Client i
waits `i x 56 / N` seconds before its first connect. Disconnects and
connects then spread evenly over the 56-second cycle. With ten clients, a
connect or disconnect happens about every 3 s, about a third of the clients
are waiting at any moment, and live traffic never stops.

**On every reconnect:**

- No denial and no full result. The client state never goes negative
  (`sdk/rocketnet.h:519-525`).
- No leak from the old connection. Sequences restart at 0, and every message
  carries the new cycle byte, so an old message fails at byte 2.
- The server's event log shows the old slot freed (disconnect or timeout)
  before the new connect is assigned.
- The server's resident memory is flat across cycles (section 7).

## 7. Metrics and pass

**Result lines.** Each process writes tab-separated lines to `--results`:
`cell`, `bench`, `counters`, `rss`, `event` and `fail`. The merge (section
12) joins them.

**Per bench x direction x mode x channel x size bucket, per window:** sent,
received, verified, failed, lost (unreliable), superseded (sequenced),
missing and out-of-order (reliable, both 0), duplicate (0), refused-on-send
(`over`), paced.

**Per bench:**

- connect successes and times;
- RTT p50 and p99 and loss, from `rocketnet_client_transport_stats` each
  second (`sdk/rocketnet.h:830-842`);
- cycles by disconnect kind;
- reconnect successes and times (p50, p99, max);
- every denial and every full result;
- the final clean disconnect.

**Internal counters on both sides, at start, every minute and at exit.**
Every counter goes out by name, as `server.c:95-106` already prints them.
The table picks out:

| Figure | Counters |
|---|---|
| resends | `MESSAGE_RESENT` |
| ack-window wraps | sent-history and message-window wraps, computed (section 6); `SEND_HISTORY_LOST` (`:514`) and `SEND_HISTORY_OVERFLOW` (`:395`, must be 0) |
| fragment and block drops | `FRAGMENT_PAYLOAD_ABANDONED`, `CLIENT_FRAGMENT_PAYLOAD_ABANDONED` (`:350`, `:372`), `MESSAGE_BAD_BLOCK` (must be 0) |
| backpressure | the runner's `paced`; `MESSAGE_WINDOW_STALL` (`:452`); `MESSAGE_SEND_QUEUE_FULL` and `MESSAGE_SEND_STORE_FULL` (0) |
| decrypt and replay refusals | `PAYLOAD_DID_NOT_OPEN`, `PAYLOAD_ALREADY_RECEIVED` (`:161-162`) and their siblings for every packet type; `XDP_KERNEL_DROPPED_REPLAYED` (`:570`) |
| XDP kernel verdicts | `XDP_KERNEL_REDIRECTED` (`:543`) and every `XDP_KERNEL_DROPPED_*` |
| conservation | `rocketnet_counters_conserved` on both sides at every snapshot (`soak.c:54`) |

**Resident memory.** Each program reads its own resident set each minute:
`/proc/self/statm` on Linux, `task_info` on macOS, `GetProcessMemoryInfo` on
Windows. The bound: after the first full cycle (the warm-up), the peak minus
the reading at the end of cycle 1 stays within 2 MiB for each client and for
the server. The library allocates nothing after create (`docs/CHANNELS.md`
1, R1), and the XDP UMEM is taken once at create
(`sdk/rocketnet_server_socket_linux.cpp:646-655`). Growth past 2 MiB is a
FAIL, named with its curve.

**max_clients.** The library constant `ROCKETNET_MAX_CLIENTS` is 1,000
(`sdk/rocketnet.h:44`). At `20d8b35` the config table's `max_clients`
default is 64 (`sdk/rocketnet.h:923`); at the base tip `962e3a4` another card
has made it 1,000. `server.c` sets 256 (`server.c:42`), which overrides
either default, and `soak.c` sets 16 (`soak.c:105`). The run never relies on
a default. It passes `--max-clients 1000`, and the server prints
the value it created with. With about ten clients, every reconnect finds a
free slot even while the server is still timing out the old one.

**Failures are recorded with their reason.** Each of these is recorded
with the denial code or client state (`sdk/rocketnet.h:519-525`), which side
timed out, the counters at that moment and a 50-line log excerpt:

- any failed connect;
- any denial;
- any disconnect other than the scheduled clean disconnect or silent drop
  (a `rocketnet_message_*_disconnect_reason` other than NONE,
  `sdk/rocketnet.h:1812`, `:1882`; a timeout on a live connection; a kick).

**The pass rule.** The run passes only when every row holds, for the whole
run, on every bench:

| # | Rule |
|---|---|
| P1 | Every message received verifies. `failed` = 0 in every cell. |
| P2 | Reliable modes: missing = 0, out-of-order = 0, duplicate = 0 in every cell. Every receipt mode: exactly one receipt per serial, no LOSS on 6 and 7. |
| P3 | Zero failed connects, zero denials, zero unexpected disconnects. |
| P4 | Every over-maximum start refused with its named counter, delta 1. |
| P5 | No crash. Every process exits 0 after its final clean disconnect. |
| P6 | Memory flat as above, on the server and every client. |
| P7 | Conservation holds on both sides at every snapshot. |
| P8 | Every nonzero counter is explained by one row of the table below. Every refusal-class counter not in that table reads 0. |
| P9 | Coverage: each (direction, mode, bucket) cell sent at least once per window. Each boundary value of B sent at least once per (direction, mode) over the run. 10 x W numbers and 4 history wraps (estimated, section 6) per window per direction. |
| P10 | The server's driver read back `xdp`, and `max_clients` read back 1,000. |

**The nonzero counters expected, with their mechanism and size:**

| Counter | Mechanism | Order of size |
|---|---|---|
| `MESSAGE_UNRELIABLE_OVER_BUDGET` | offered load is 110% of the budget | about 10% of unreliable sends |
| `MESSAGE_SEND_REFUSED` | the over-maximum starts | exactly the runner's count |
| `MESSAGE_UNRELIABLE_SEND_FAILED` | the over-maximum starts on codes 0 and 1 | exactly the runner's count |
| `MESSAGE_WINDOW_STALL` | the W or ring bound is reached under load | many per window on the internet path |
| `MESSAGE_RESENT`, `FRAGMENT_PAYLOAD_ABANDONED`, `CLIENT_FRAGMENT_PAYLOAD_ABANDONED`, `SEND_HISTORY_LOST`, `MESSAGE_DUPLICATE`, `MESSAGE_SEQUENCED_SUPERSEDED`, `MESSAGE_SEQUENCED_STALE`, `MESSAGE_RECEIPT_LOSS` (mode 5 only) | path loss and reorder | near 0 on the LAN; proportional to loss on the internet |
| `MESSAGE_UNRELIABLE_DELIVERY_FULL` | a burst past 256 unreliable deliveries in one update (`sdk/rocketnet.h:1541`) | rare; each occurrence is listed |
| `CLIENT_TIMED_OUT` | the silent drops | exactly the silent drop count |
| the handshake and lifecycle EXPECTED counters | connects and disconnects | proportional to cycles |
| `XDP_KERNEL_REDIRECTED` | every server-bound packet | all of them |

## 8. Hosts

- **The server** runs only on the dedicated test server. No bench ever
  starts a server: the orchestrator's server steps take the test server's
  inventory row and refuse any other.
- **Clients** run on every Linux bench, on the Windows side of the
  Linux-on-Windows bench (a native build, driven through interop), and on the
  macOS bench. Each client uses its platform's default client driver: gso is
  server-only, so Linux and Windows clients run portable, and the macOS
  client runs msgx (`sdk/rocketnet_drivers.inc:5-9`; `USAGE.md:541`).
- **The inventory.** This document names platform shapes only. Hosts come
  from an inventory file outside the repository, named by
  `ROCKETNET_LIVE_INVENTORY`. It has one row per machine: `label`,
  `ssh-name`, `role` (server or client), `shape` (linux, windows-interop or
  macos), `site` (studio or datacenter), `path` (lan or internet),
  `cap-kbps` (the client's total, both directions combined: 1,000 for a
  studio-site client and 100,000 for a datacenter client, RULINGS (b) and
  (d)), `nic`, `address` and `queues` (server row only). Every studio-site
  bench, both sides of the Linux-on-Windows bench included, crosses the one
  shared studio uplink; the datacenter benches do not. The repository commits
  only a sample with placeholder names.

## 9. The XDP server and its NIC

**The owner's ruling of 2026-10-18 replaces the card's rule here.** The test
server has one usable NIC: the one that carries its public address and
ssh. XDP binds that NIC. The earlier rule (2026-10-16, after the machine
went dark) forbade XDP on the uplink. This run is the deliberate exception,
under the safety procedure below. The line, for the owner: **NIC: the
uplink, by the 2026-10-18 ruling; not blocked.**

**How the driver finds the NIC.** It looks up the interface that carries the
server's public IPv4 address (`sdk/rocketnet_server_socket_linux.cpp:444-475`).
The address must be IPv4: the pre-flight refuses anything else (`:706-718`).
The driver binds that interface and no other. The attach is native, falling
back to SKB mode (`:1335-1352`). Each queue binds an AF_XDP socket. The bind
asks for no explicit mode, so the kernel gives zero copy where the driver
supports it and copy otherwise (`:1487-1493`, see section 15). The run
records which it got.

**The pre-flight.** It refuses unless the machine is provisioned:

- root (R1, `:697-701`);
- the interface UP with the address (R2);
- no XDP program already attached (R3, `:722-745`);
- IRQ pinning, busy poll, SMT off and the queue count (R4-R7, `:804-990`).

`scripts/rocketnet-provision` is the setup, hand-run once for the game
(`scripts/rocketnet-provision:8-16`). The run uses only its `verify` mode,
which mutates nothing (`:98`). It never runs provision mode on this NIC.
Provision mode unloads XDP, rewrites IRQ affinity and runs `ethtool -L`,
which resets the NIC's queues under the ssh session (`:54-66`, `:80-85`).
If verify fails, the run stops and the owner decides.

**The NIC driver.** It must be the out-of-tree i40e, built from source
(B3, `scripts/rocketnet-provision:39-45`). The run records `ethtool -i <nic>`
read-only (driver, version, firmware) and refuses to start if the version
string is the in-tree driver's: the in-tree i40e reports the running
kernel's version (the same string as `uname -r`), and the out-of-tree build
reports its own release number.

**Proof that XDP passes everything that is not ours** (`server_xdp/server_xdp.c`):

- **Port or address mismatch:** a UDP datagram to any other port, or to any
  other destination address, returns `XDP_PASS` (`:126-128`). That is the
  server's port-and-address gate.
- **Unconfigured:** with no configuration, or port 0, every UDP frame passes
  (`:271-275`, `:301-305`).
- **Non-UDP IPv4:** any non-UDP IPv4 frame, or one with IP options
  (`ihl != 5`), skips the UDP branch at `:263` and reaches `return XDP_PASS`
  at `:336`. That covers ssh (TCP), ICMP and anything else.
- **IPv6:** IPv6 with an extension header passes at `:320-329`. Any other
  non-UDP IPv6, such as ICMPv6 and neighbour discovery, passes at `:331-336`.
- **Non-IP frames,** ARP above all, pass at `:336`.
- **The overlay:** the tailnet's WireGuard datagrams go to their own port,
  not the server's, and pass at `:126-128`.

The program returns `XDP_DROP` in three places outside our port:

- an IPv4 frame too short for its IPv4 header (`:257-261`);
- an IPv6 frame too short for its IPv6 header (`:287-291`);
- a frame for our port that fails the server's own checks
  (`:137-182`, `:220-225`, `:240`).

The first two are malformed frames that no stack could deliver. So
everything the machine needs (ssh, the tailnet, ARP, ICMP, DNS) passes.
The program never touches egress. The server's port for the run, 40000, is
not the overlay's.

**Deployment.** One server per machine, about 3.9 GB of XDP memory, an
accepted cost (`CMakeLists.txt:49`, the 4,096 MiB budget). The server binds
`<public IPv4>:40000`. The queue count comes from the NIC's combined
channels as provisioned, read-only, and `--num-xdp-queues` passes it.

**Safety procedure on the test server**, in order. Every step is a script
file run over ssh under a timeout (section 12):

1. **Read-only checks.** `ip -o link`; `ethtool -i <nic>` recorded;
   `ip -d link show dev <nic>` shows no `prog/xdp`; `xdp-loader status`
   shows nothing on the NIC; `rocketnet-provision <nic> <queues> verify`
   exits 0; no `/sys/fs/bpf/rocketnet_xdp_*` pin root left over. Any miss
   stops the run before anything is attached.
2. **Arm the dead-man timer** before loading anything. The command needs no
   file on the machine, so a missing script can never turn it into a no-op:
   `systemd-run --unit=rn-live-deadman --on-active=15min /bin/sh -c "systemctl kill rn-live-server; ip link set dev <nic> xdp off; ip link set dev <nic> xdpgeneric off"`.
   The second `off` covers an attach that fell back to SKB mode (`:1345`).
   `server-arm.sh` writes the NIC name in, then reads the timer back with
   `systemctl list-timers rn-live-deadman` before it returns. The
   orchestrator cancels the timer only after it has confirmed that no
   program is attached (step 7).
3. **Start the watchdog,** `systemd-run --unit=rn-live-watchdog`. Every 2 s
   it checks two things: the heartbeat file the orchestrator touches over
   ssh every 5 s, and the server unit. If the heartbeat is older than 30 s,
   or the server unit is gone while a program is attached, it runs the same
   three commands as the dead-man timer. The heartbeat stops when ssh drops
   and also when the orchestrator dies without its trap, so either way the
   watchdog acts. This is the server-side watchdog the card asks for, and
   `RNLiveFleetRun` models it (`WatchdogFires`, `OrchestratorDies`).
4. **Load the server,** `systemd-run --unit=rn-live-server`, with
   `ROCKETNET_SERVER_DRIVER=xdp`, `ROCKETNET_SERVER_PRINT_COUNTERS=1` and the
   matrix flags.
5. **Within 10 s of the load,** open a fresh ssh connection (a new TCP
   connection, no shared control master) and run `true`. If it fails, the
   orchestrator stops sending heartbeats, and the watchdog detaches inside
   30 s. The dead-man timer covers a watchdog that never ran.
6. **Wait for `listening`** in the status file. Check the driver read-back
   `xdp`, `max_clients` 1,000, and one LAN client connecting, before the
   other clients start.
7. **Teardown, on every exit path (trap):**
   1. SIGINT the server. Its stop and destroy detach the program
      (`server.c:89-108`; `sdk/rocketnet_server_socket_linux.cpp:1627-1641`).
   2. Run `ip link set dev <nic> xdp off` and `xdpgeneric off` regardless.
   3. Confirm that `ip -d link show` and `xdp-loader status` show no program
      on the NIC.
   4. Run `rocketnet-provision sweep`.
   5. Stop the watchdog. Then cancel the dead-man timer.
   6. Confirm ssh once more with a fresh connection.

   A crashed server never ran its destroy, and the libxdp attachment
   outlives the process. That is why step 2 never trusts the destroy
   (`RNLiveFleetRun`'s `BrokenXdpLeftAttached`).

## 10. Addresses and firewall

- **The address every bench dials** is the test server's public IPv4 and
  UDP port 40000. The token carries it (`sdk/rocketnet_connect_token.cpp:288-302`),
  and it is the address XDP matches (`server_xdp/server_xdp.c:277`).
- **No tailnet or private address.** Overlay traffic arrives on the overlay
  interface, not on the address the driver binds. A token naming it would be
  refused.
- **LAN benches** reach the public address across the local network.
  **Internet benches** reach it across the internet.
- **Replies go through the gateway.** The xdp driver addresses every frame
  it sends to the default gateway's MAC address
  (`sdk/rocketnet_server_socket_linux.cpp:1141-1158`; override
  `ROCKETNET_GATEWAY_ETHERNET_ADDRESS`, `:255-274`, `USAGE.md:446`). A LAN
  bench on the same segment therefore gets its replies through the gateway,
  not directly. Step 6 of section 9 proves one LAN client's round trip first.
  If it fails, the run stops with that finding (section 15).
- **The host firewall** never sees our port. XDP redirects in the driver,
  before netfilter, so no host rule is needed. Upstream filtering in front of
  the test server must pass UDP 40000 from the internet benches. The run
  reads, never changes, any firewall. Opening a rule is an access-control
  change, and that is the owner's to make, before the run.

## 11. Tokens

- **Minted at each connect, in the client process.** Each client mints its
  own development token just before each connect, with
  `rocketnet_write_dev_connect_token(address, rocketnet_random_uint64(), time(NULL), token)`,
  as `client.c:46` does. It is never minted ahead of time and never shipped
  between machines.
- **Lifetime and identity.** The token lives 30 s
  (`ROCKETNET_DEV_CONNECT_TOKEN_EXPIRE_SECONDS`,
  `sdk/rocketnet_connect_token.h:165-171`), and the client id is random per
  connect. A reconnect after 20 s therefore always uses a token minted 0 s
  earlier, and no token is ever reused (`docs/SESSION.md` 1.6).
- **Clock-skew check.** Before the run, the orchestrator reads
  `date +%s.%N` over ssh from the test server and from every bench. It takes
  the midpoint of each round trip and computes each bench's offset from the
  server. A bench more than 2 s off is refused: it would shorten its tokens'
  life or present a token from the future. The offsets go in the report.
  Each client re-reads its clock at every mint.
- **Keys.** The keys are the published development keys. No key, and
  nothing derived from one, appears in any log or result line.

## 12. Orchestration

**One script,** `scripts/rocketnet-live-xdp`, run on the coordinator's
machine. There it runs only ssh and the merge. Every remote step is a script
file under `scripts/live-xdp/`. The orchestrator sends it with
`ssh <host> bash -s -- <args> < scripts/live-xdp/<step>.sh` (on the Windows
side, through the bench's interop), under `timeout`. It never sends an
inline quoted command string.

The steps, in order:

1. **Plan.** Read the inventory. Compute the stagger, the attached-time
   bound (refuse if over 13 min), the run byte bound (refuse if over 20 GB)
   and the studio-uplink bound (refuse if over 1 GB), both from each row's
   total cap under RULINGS (d). Refuse any client whose cap is missing or
   above its site's 1,000 kbps (studio) or 100,000 kbps (datacenter) total.
   Print the plan, every client's cap and both bounds.
2. **Build.** On each machine in parallel, run `bench-build.sh <commit>`:
   clone from GitHub into `~/rocketnet-live/<commit>`, check out the one
   named commit, and build with the card's build bound. On Windows the build
   is native, through interop; on macOS it is native. Each machine prints
   `git rev-parse HEAD`. The orchestrator refuses a mismatch. Nothing is
   copied between machines.
3. **Skew** check (section 11).
4. **Server pre-flight,** dead-man timer, watchdog, load, ssh check, listen
   (section 9, steps 1 to 6). A heartbeat loop starts and runs until
   teardown. Root is used only for the steps of RULINGS (c): load and unload
   the program, arm and cancel the dead-man timer, and start and stop the
   server. Those run as non-interactive `sudo -n <script>` on the copied
   script file, after `sudo -n true` proves the grant; the host's sudo is
   sudo-rs, so the environment list is passed with
   `--preserve-env=<list>` and never `-E`. If `sudo -n true` fails or asks
   for a password, the run stops with not-done and no password is supplied.
5. **Clients.** Start one per bench with `bench-client.sh`, under
   `systemd-run --user` on Linux, `nohup` on macOS and `start /b` on
   Windows. Each client has its label, stagger, its row's total cap (500
   kbps each way on the studio site, 50,000 kbps each way in a datacenter)
   and the seed.
6. **Watch.** Every 30 s, read each client's last status line and the
   server's. A client or server exit before plan, or a `fail` line, begins
   teardown at once.
7. **Teardown** (section 9, step 7), first, as soon as the clients are done
   or anything failed, and on every exit path: `trap` on EXIT, INT, TERM and
   HUP. It stops every client it started, then the server, then detaches,
   sweeps, stops the watchdog and cancels the dead-man timer, in that order.
   Nothing else runs while a program is attached.
8. **Collect,** after the detach is confirmed. `bench-collect.sh` prints
   each machine's result file to stdout, a few hundred KB of text, into the
   coordinator's run directory. Data flows inward only.
9. **Merge.** `scripts/live-xdp/merge.sh` (awk) writes the matrix, the
   counter table and the pass table P1 to P10 as `matrix.tsv`,
   `counters.tsv` and `PASS.md`. It exits 0 only on PASS.

The orchestrator has a `selftest` mode, as `rocketnet-provision` has
(`scripts/rocketnet-provision:103-109`). It drives the whole flow against a
stub `ssh` that replays captured output: no host, no root. It checks each
exit path against `RNLiveFleetRun`, and CI runs it.

## 13. Model

`tla/RNLiveFleetRun.tla` (instance `tla/MCRNLiveFleetRun.tla`, group
`rnlivefleetrun` in `tla/CASES.tsv`, notes in `tla/README-RNLiveFleetRun.md`)
models the run's lifecycle.

**State:**

- the orchestrator's step;
- the server: absent, starting, listening or stopped;
- where the server runs;
- whether XDP is attached;
- ssh up or down, and for how long;
- how long XDP has outlived its server;
- the watchdog, and the dead-man timer and its age;
- each client's phase: built, connecting, running, disconnected-clean,
  dropped-silent, waiting, failed or done;
- each client's cycle, its wait, and its live and previous slot;
- whether results were collected.

**Actions:**

- the orchestration steps: ArmDeadman, LoadServer, SshCheckPasses (the
  fresh ssh connection right after the load, before the listen line),
  SshCheckFails, ListenSeen, BeginTeardown, StopClients, StopServer,
  DetachXdp, Finish (collect only after the detach). The 10 s bound on the
  ssh check is not modelled; its place in the order is;
- the server and its timers: Listen, DisconnectArrives, ServerTimesOut,
  WatchdogFires, DeadmanFires;
- each client's cycle: StartClient, Connected, EndWindow, BeginWait,
  Reconnect, FinalDisconnect, LosesServer;
- the outside events, bounded by `MaxFaults`: SshDrop, SshBack, ServerCrash,
  ClientCrash, Interrupted (the trap runs), OrchestratorDies (killed without
  its trap: no teardown and no more heartbeats);
- Tick. The timers that must fire on time are urgent and block it.

**Invariants:**

- `XdpNeverOrphaned`: XDP is never attached while the server is gone, or
  while the orchestrator's heartbeat is stopped (ssh down or the
  orchestrator dead), for longer than the watchdog bound.
- `TeardownDetaches`: a finished run leaves no server running and no program
  attached.
- `XdpOnlyUnderDeadman`
- `NoClientBeforeListen`, `NoBenchServer` and `ReconnectAfterTwoTimeouts`:
  structural, each held by an action's guard and stated so the code card
  keeps the guard;
- `ReconnectIntoFreedSlot`: a client's live connection never shares the
  server with its previous one.
- `NeverDenied`: with `max_clients` at least the client count, no connect is
  denied.

**Liveness:** `EndsStoppedAndDetached`, every run ends with the server
stopped and XDP detached.

**Witnesses.** Each reversed witness violates exactly its declared
invariant:

- `MCRNLiveFleetRunBrokenXdpLeftAttached.cfg`: teardown trusts the server's
  destroy, and a crash during load reaches done with XDP attached. It fails
  `TeardownDetaches`.
- `MCRNLiveFleetRunBrokenServerOrphaned.cfg`: no watchdog, and an ssh drop
  or an orchestrator killed right after the load leaves XDP attached past the
  bound. It fails `XdpNeverOrphaned`.

`tla/README-RNLiveFleetRun.md` has the runs and the two counterexamples,
checked by hand against this design.

## 14. File plan

**Card 1: `rocketnet-live-message-matrix-functional`.** The shared verify and
matrix code, and a loopback functional test that CI runs on every platform.

- Creates:
  - `test/live_matrix.h`: sections 3 to 5, C99 `static inline`.
  - `test_unit_live_matrix.cpp`: known-answer vectors for the length and
    byte rules, every boundary value, the tiny-prefix rules, and the
    duplicate, wrong-cycle and wrong-channel failures. No socket, no clock,
    time starting at 1000.0.
  - `functional/live_matrix.cpp`: both message objects in one process over
    the loopback driver, with time passed as an argument. It runs the full
    matrix for two windows (one clean disconnect, one silent drop), the
    over-maximum refusals, and the E_m and receive-channel assertions of
    sections 4 and 5.
- Touches: `CMakeLists.txt` (the functional source list, `:433-438`; the
  unit glob at `:295` already picks the new file up).
- Gate: `make && ./bin/test && ./bin/functional`.

**Card 2: `rocketnet-live-xdp-fleet-runner`.** The client runner, the server
mode and the orchestration script.

- Touches:
  - `client.c`, `server.c`: the `--matrix` mode and the flags of section 2.
  - `sdk/rocketnet_connect_token.h`, `sdk/rocketnet_connect_token.cpp`:
    `ROCKETNET_DEV_CONNECT_TOKEN_TIMEOUT_SECONDS`, replacing the literal at
    `:285`.
  - `docs/LIVE-XDP.md`: as built.
  - `tla/COVERAGE.tsv`: `live-fleet-run` to current, naming the script's
    functions.
  - `tla/README-RNLiveFleetRun.md`
- Creates:
  - `scripts/rocketnet-live-xdp`
  - `scripts/live-xdp/server-preflight.sh`
  - `scripts/live-xdp/server-arm.sh` (the dead-man timer and the watchdog)
  - `scripts/live-xdp/server-start.sh`
  - `scripts/live-xdp/server-teardown.sh`
  - `scripts/live-xdp/bench-build.sh`
  - `scripts/live-xdp/bench-client.sh`
  - `scripts/live-xdp/bench-collect.sh`
  - `scripts/live-xdp/merge.sh`
  - `scripts/live-xdp/inventory.sample`
- Gate: `make && ./bin/test && ./bin/functional && scripts/rocketnet-live-xdp selftest`.

**Card 3: `rocketnet-live-xdp-fleet-run`.** One real run, and its results.

- Creates `docs/LIVE-XDP-RESULTS.md` (the plan printed, the matrix summary,
  the pass table, the counters table, the NIC driver line, the zero-copy or
  copy mode, the skew offsets, the internet byte total) and nothing else.
  The raw result files stay in the coordinator's run directory.
- Gate: `scripts/rocketnet-live-xdp run --inventory "$ROCKETNET_LIVE_INVENTORY" --commit <sha>`
  exits 0, and the teardown receipt shows no program attached and ssh up.

## 15. Findings for the owner

1. **NIC.** The test server has only its uplink, so the card's section-9
   rule would read `NIC: blocked`. The owner's 2026-10-18 ruling takes the
   uplink under the dead-man timer, the watchdog, the 10-second ssh check and
   the verify-only pre-flight. The design follows the ruling. RULINGS (a)
   makes it cover every live XDP test on the test server, not only this run;
   worst case the owner can force-reboot the test server from its provider's
   console.
2. **max_clients.** The constant is 1,000 (`sdk/rocketnet.h:44`). The table
   default was 64 (`sdk/rocketnet.h:923` at `20d8b35`) and is 1,000 at the
   base tip `962e3a4`. But `server.c` sets 256 (`server.c:42`), so the
   example server never gets the default either way. The run sets 1,000
   explicitly with `--max-clients` and changes no default.
3. **Stale comments about enabled types.** `sdk/rocketnet.h:1708-1716` lists
   five enabled types, and `docs/CHANNELS.md` 7.5 says {0, 3}. The code
   enables all eight (`sdk/rocketnet_channel.h:59`,
   `sdk/rocketnet_channel.cpp:189`). This is a doc fix, outside this card's
   paths.
4. **The dev token's timeout is a literal 10**
   (`sdk/rocketnet_connect_token.cpp:285`) with no name. The run must read
   2 x timeout from a constant, so the need
   `rocketnet-dev-token-timeout-is-a-named-constant` adds one (section 6);
   the runner card does not.
5. **No soft backpressure at the message API.** A send the region has no
   room for ends the connection (`sdk/rocketnet.h:1719-1724`). So the run's
   backpressure figure is its own pacing ledger, and `MESSAGE_SEND_QUEUE_FULL`
   at any value is a FAIL, not a measured backpressure. A soft "would not
   fit" query would let applications pace without a ledger. That is a
   possible library card, not this run's.
6. **XDP replies go to the gateway's MAC address**
   (`sdk/rocketnet_server_socket_linux.cpp:1141`). On-link LAN peers depend
   on the gateway forwarding back onto the segment. The run proves one LAN
   round trip before it starts.
7. **The AF_XDP bind mode.** `xsk_config.xdp_flags = XDP_ZEROCOPY`
   (`sdk/rocketnet_server_socket_linux.cpp:1489`) puts a bind-flag constant
   in the attach-flags field. With `XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD`
   (`:1491`) it does not request zero copy at bind; the kernel picks zero
   copy where the driver supports it and copy otherwise. The run records
   the mode it got. A fix belongs to its own card.
8. **The provision script.** `rocketnet-provision` in provision mode mutates
   the NIC, so on the uplink the run uses verify only. If verify fails (the
   pre-flight refuses without R4 to R7), the run stops for the owner.
9. **Root on the test server.** The xdp driver needs root
   (`sdk/rocketnet_server_socket_linux.cpp:697-701`). The workers' standing
   rule is no sudo. RULINGS (c) grants root for exactly three steps and no
   other: loading and unloading the XDP program, arming and cancelling the
   systemd-run dead-man timer, and starting and stopping the server. The
   orchestrator runs them as non-interactive `sudo -n <script>` on the
   landed script files (sudo-rs: `--preserve-env=<list>`, never `-E`) after
   `sudo -n true` proves the grant; a password prompt stops the run with
   not-done.
10. **The cap is a total.** RULINGS (b) and (d) settle it: every per-client
    cap is the client's total, both directions combined. A studio-site client
    is capped at 1 Mbit/s total (500 kbps each way), a datacenter client at
    100 Mbit/s total (50,000 kbps each way), and the run is bounded at 20 GB
    in all and 1 GB across the studio uplink.
11. **The 24-bit reliable number wrap** cannot happen inside the 15-minute
    dead-man timer at these rates. The run does not claim it (section 6).
12. **Soak's refusal rule cannot hold under full load.** `soak.c:60-68`
    fails any nonzero refusal. Here
    `MESSAGE_UNRELIABLE_OVER_BUDGET`, `MESSAGE_WINDOW_STALL` and the
    over-maximum refusals move by design, so section 7 replaces that rule
    with an exact allow-list.
13. **No cumulative wire counters.** The transport stats are trailing
    per-second rates (`sdk/rocketnet.h:830-839`, `:1284-1292`), and no
    counter counts packets or bytes sent. The run's internet byte total and
    its sent-history wrap count are therefore integrals of samples, labelled
    approximate, beside the exact bound the send budget gives. A cumulative
    packets-sent and bytes-sent pair on the counter surface would make both
    exact. That is a possible library card.
14. **The server's send budget is one value at create**
    (`sdk/rocketnet.h:1823-1826`). Per-path caps need the per-slot setter
    (`sdk/rocketnet.h:1879`) and a way for the server to learn each client's
    path. The run uses a HELLO message (section 6). Under RULINGS (d) the
    server creates with the lowest cap, 500 kbps, and raises a slot only to
    that slot's inventory cap, so no client is ever over its cap.

## Running it

On the coordinator's machine, from the checkout:

    scripts/live-xdp-fleet --dry-run --inventory scripts/live-xdp-fleet.inventory.example
    scripts/live-xdp-fleet --inventory "$ROCKETNET_LIVE_INVENTORY" --commit <sha>

The dry run prints the plan, every client's cap, the run bound and the
studio-uplink bound of RULINGS (d), and every per-host build, start, collect
and teardown step, including every XDP SAFETY step, with the client and
server command lines in full. It contacts no host and ends with
`dry-run: no host contacted`. It refuses an inventory with a studio-site cap
above 1,000 kbps or a plan over 20 GB in all or 1 GB across the studio
uplink.

The real run copies `scripts/live-xdp-fleet-remote` to each machine and runs
its `build`, `start`, `stop`, `collect` and `restore` subcommands there; a
step is never an inline quoted command string over ssh. Root on the test
server is used only for the steps of RULINGS (c), as non-interactive
`sudo -n <script>` on the copied script file after `sudo -n true`. The
example inventory holds placeholder hosts only; the real one lives outside
the repository and is named by `ROCKETNET_LIVE_INVENTORY`.

Live run attempt 2 (`rocketnet-live-xdp-fleet-run.w2`, staged at `4d58844`)
was not run: the companion runner card that adds `--matrix` mode to
`client.c`/`server.c` (invoked by `scripts/live-xdp-fleet-remote start`) and
lands `scripts/live-xdp-fleet-merge` (invoked at the end of
`scripts/live-xdp-fleet`) has not landed on `sprint/rocketnet-proof`. The
dry run passes and shows every XDP SAFETY step with the plan inside the
RULINGS (d) bounds, but the real run cannot start (the server and client
start commands pass `--matrix` to binaries without it) and the merge step
cannot run, so the results section of the DONE-WHEN gate cannot be
produced. The card is reported not-done with this reason.
