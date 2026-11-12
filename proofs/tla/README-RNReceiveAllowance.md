# RNReceiveAllowance: the server's per-client receive allowance

`proofs/tla/RNReceiveAllowance.tla` is the byte allowance a server keeps for
each client slot when `per_client_receive_kbps` is set. The code is
`rocketnet_server_receive_allowance_take` and its call in
`rocketnet_server_process_sealed_packet` (sdk/rocketnet_server_protocol.cpp),
the fill when a slot is assigned in `rocketnet_server_process_connection_response`,
the rate and capacity in sdk/rocketnet_server_protocol.h, and the slot's
zeroing in `rocketnet_server_connection_reset`.

The credits are `rocketnet_server_receive_allowance_take_packet` and
`rocketnet_server_receive_allowance_start` beside it, and the ack credit the
server's own standalone ack spends in `rocketnet_server_protocol_write_ack_if_due`.

`proofs/tla/MCRNReceiveAllowance.tla` is the module every case runs. Three
passing files: `MCRNReceiveAllowance.cfg` (the bytes),
`MCRNReceiveAllowanceCredit.cfg` (the credits, a correct client) and
`MCRNReceiveAllowanceFlood.cfg` (the credits, a flooder). The eight
`proofs/tla/MCRNReceiveAllowanceBroken*.cfg` files are the reversed witnesses.
The cases are rows of `proofs/tla/CASES.tsv` (group `rnreceiveallowance`) with
measured records in `proofs/tla/RUNS.tsv`.

## The rule

- **The rate.** Twice the setting (`ROCKETNET_RECEIVE_ALLOWANCE_FACTOR`), in
  wire bytes a second, 125 bytes a second per kbps: the unit and the bytes the
  client's send budget counts. 0 selects `ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS`;
  `ROCKETNET_SEND_KBPS_UNLIMITED` keeps no allowance.
- **The capacity.** The rate times the connection's timeout in seconds, plus
  one maximum packet (`ROCKETNET_SEND_BUDGET`, the least depth of the client's
  own bucket). Full when the slot is assigned.
- **The refill.** From the time argument of the update that read the packet;
  only time past the last refill adds.
- **The refusal.** After the packet opens and its sequence is marked, before
  the ack block. A packet that fits spends its wire bytes; one that does not is
  dropped, counted (the type's `OVER_ALLOWANCE`, and the slot's
  `receive_refused_packets` and `receive_refused_bytes`), and spends nothing.
  Nothing ends the connection.
- **The ack credit.** A standalone ack is sent only while the client owes an
  ack, and it owes one only for a packet of the server's that asks for one
  (a keep-alive, a payload, a disconnect; not the server's own standalone
  ack). So the server credits one standalone ack for each such packet: the
  session sequences it has taken for the connection (`send_sequence_session`)
  less a mark that starts at the connection's first sequence and moves up by
  one for each of the server's own standalone acks and each ack credited. An
  ack past the credit pays bytes. The credit is not capped: an ack held up
  for the timeout and delivered late still has its packet's credit. A
  flooder's acks are bounded by what the server sent it.
- **The control tokens.** A keep-alive goes only on the client's send tick
  (`ROCKETNET_DEFAULT_SEND_RATE`, ten a second) and a disconnect ends the
  connection. Tokens refill at twice that rate from the time argument, like
  the bytes, up to twice the rate times the timeout plus two (one for the
  keep-alive that starts an interval, one for the disconnect), and start full.
  A keep-alive or disconnect past the tokens pays bytes.

## The instance

One correct client `c1` and one flooder `f1`; the setting B = 1 byte a tick at
Factor 2 (R = 2); one maximum packet of 2, which is also the correct client's
bucket depth; a timeout of T = 2 ticks, so the correct client's packets arrive
up to one tick late and may arrive together, two in flight at most; five
ticks; two flood packets a
tick; one step back of the server's time argument; one slot reuse. The server
sends nothing and the clients send payload only (`MaxElicit = 0`,
`Kinds = {"pay"}`). The control checks every invariant. It held three packets
in flight before the credits; with the credit variables in every state TLC
takes about 1.4 times as long a state, and three would not fit the 110 s run
budget on the bench, so two.

`MCRNReceiveAllowanceCredit.cfg`: one correct client, the same setting and
packet, three ticks, up to two session packets from the server (eliciting,
received or lost at once, the worst case for the credit; or the server's own
standalone ack), all four kinds, its acks, keep-alives and disconnect at a
maximum packet each (the worst for the bytes), up to two in flight, one slot
reuse. `MCRNReceiveAllowanceFlood.cfg`: one flooder sending any kind at any
size, one a tick, five ticks, up to three session packets, one step back, one
slot reuse. The flooder instance has no correct client: the slots are one
struct indexed by the client in the code, so isolation is the first instance's
and `BrokenShared`'s.

| Configuration | Flip | Must fail | What it shows |
|---|---|---|---|
| `MCRNReceiveAllowanceBrokenNoRefill.cfg` | `Refill = FALSE` | `UnderRateNeverRefused` | a client under 1x is refused |
| `MCRNReceiveAllowanceBrokenSmallCap.cfg` | `CapFull = FALSE` | `UnderRateNeverRefused` | the burst after a silence is refused |
| `MCRNReceiveAllowanceBrokenShared.cfg` | `PerClient = FALSE` | `Isolated` | refusing client A changes client B |
| `MCRNReceiveAllowanceBrokenTimeBack.cfg` | `Monotone = FALSE` | `NoMint` | time going back mints bytes |
| `MCRNReceiveAllowanceBrokenNoAckCredit.cfg` | `AckCredit = FALSE` | `UnderRateNeverRefused` | a correct client's acks are refused |
| `MCRNReceiveAllowanceBrokenNoControl.cfg` | `Control = FALSE` | `UnderRateNeverRefused` | a correct client's keep-alives are refused |
| `MCRNReceiveAllowanceBrokenFreeAcks.cfg` | `CreditBound = FALSE` | `AcksBounded` | a flooder's acks are never refused |
| `MCRNReceiveAllowanceBrokenServerAckCredits.cfg` | `ServerAckSpends = FALSE` | `AcksBounded` | the server's own acks pay for a flooder's |

## What is not modelled

Packet sizes beyond one maximum packet: the credits pay by count, not bytes,
so an ack of any size is one credit. The client's update rate: it changes how
many acks a client sends only within what it owes. Measured on the loopback
transport before the credits (2026-11-11): a client sending nothing while it
received a 10% lossy stream at the server's default 10 Mbps, updated at 100
Hz, sent about 680 kbps of standalone acks, and a setting of 64 kbps refused
most of them; with the credits, `tests/test_unit_receive_allowance.cpp`
streams 60 s at 4, 8, 64, 256 and 1000 kbps under 0, 1, 10 and 30% loss and
refuses nothing.

## Reading the counterexamples against the code

Run on amd-3, 2026-11-11, pinned tlacheck 0c97d595 and the pinned JAR, one
110 s shard per case, two workers. The control passes with 5,199,712 distinct
states in 43 s; each witness exits 12 on its declared invariant. Each trace was
read by hand against the C:

- **BrokenNoRefill** (`UnderRateNeverRefused`, 411,384 distinct). The correct
  client sends its bucket and then one byte a tick; the allowance starts at 6
  and is never refilled, so at tick 5 its sixth byte arrives over an empty
  allowance and is refused. In the C the refill is the `time > allowance->time`
  block of `rocketnet_server_receive_allowance_take`; with it removed the unit
  tests `at_the_rate_never_refused`, `burst_after_silence_not_refused` and the
  public `correct_client_never_refused` go red.
- **BrokenSmallCap** (`UnderRateNeverRefused`, 1,331 distinct). The client
  sends 1 byte at tick 0 and 2 at tick 1, both delivered at tick 1; a cap of
  one tick (2) holds the first and refuses the second. The C capacity is
  `rocketnet_server_receive_allowance_capacity`, the rate times the timeout
  plus `ROCKETNET_SEND_BUDGET`; cut to one second, the public
  `correct_client_never_refused` (its T-1 outage and server stall) goes red.
- **BrokenShared** (`Isolated`, 5 distinct). One flood byte spends the shared
  allowance and the correct client's allowance is no longer what its own
  packets made it. The C indexes `receive_allowances[client_index]`; indexed
  at 0, `refused_packet_does_nothing` goes red.
- **BrokenTimeBack** (`NoMint`, 107,189 distinct). The flooder drains, the
  time argument steps back from 1 to 0 and the refill credits the distance,
  then the clock moves on to 2 and credits 0 to 2 again: 11 bytes accepted
  against a bound of 6 + 2 x 2. The C adds only when `time > allowance->time`
  and leaves the refill time otherwise; refilling on a step back turns
  `time_back_and_jump_mint_nothing` red.
