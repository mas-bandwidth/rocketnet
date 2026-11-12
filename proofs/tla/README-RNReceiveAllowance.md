# RNReceiveAllowance: the server's per-client receive allowance

`proofs/tla/RNReceiveAllowance.tla` is the byte allowance a server keeps for
each client slot when `per_client_receive_kbps` is set. The code is
`rocketnet_server_receive_allowance_take` and its call in
`rocketnet_server_process_sealed_packet` (sdk/rocketnet_server_protocol.cpp),
the fill when a slot is assigned in `rocketnet_server_process_connection_response`,
the rate and capacity in sdk/rocketnet_server_protocol.h, and the slot's
zeroing in `rocketnet_server_connection_reset`.

`proofs/tla/MCRNReceiveAllowance.tla` and `proofs/tla/MCRNReceiveAllowance.cfg`
are the small instance. The four `proofs/tla/MCRNReceiveAllowanceBroken*.cfg`
files are the reversed witnesses. The cases are rows of `proofs/tla/CASES.tsv`
(group `rnreceiveallowance`) with measured records in `proofs/tla/RUNS.tsv`.

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

## The instance

One correct client `c1` and one flooder `f1`; the setting B = 1 byte a tick at
Factor 2 (R = 2); one maximum packet of 2, which is also the correct client's
bucket depth; a timeout of T = 2 ticks, so the correct client's packets arrive
up to one tick late and may arrive together; five ticks; two flood packets a
tick; one step back of the server's time argument; one slot reuse. The control
checks TypeOK, UnderRateNeverRefused, Isolated and NoMint.

| Configuration | Flip | Must fail | What it shows |
|---|---|---|---|
| `MCRNReceiveAllowanceBrokenNoRefill.cfg` | `Refill = FALSE` | `UnderRateNeverRefused` | a client under 1x is refused |
| `MCRNReceiveAllowanceBrokenSmallCap.cfg` | `CapFull = FALSE` | `UnderRateNeverRefused` | the burst after a silence is refused |
| `MCRNReceiveAllowanceBrokenShared.cfg` | `PerClient = FALSE` | `Isolated` | refusing client A changes client B |
| `MCRNReceiveAllowanceBrokenTimeBack.cfg` | `Monotone = FALSE` | `NoMint` | time going back mints bytes |

## What is not modelled

The client's send budget counts its payload packets only. Its keep-alives (only
when it has sent nothing for a tenth of a second: at most ten a second, 41
bytes) and its standalone acks (at most one per client update while it owes an
ack, sized by the ranges of the server's packets it holds, up to 1053 bytes)
are outside it, and outside this model: they are what the second share of the
rate is for. They fit while they stay under the setting. Measured on the
loopback transport (2026-11-11): at 8 kbps and up a client sending maximum
packets with a one-second timeout has nothing refused, at 4 and 6 kbps some;
a client that sends nothing while it receives a 10% lossy stream at the
server's default 10 Mbps, updated at 100 Hz, sends about 680 kbps of
standalone acks, and a setting of 64 kbps refuses most of them (lost acks, the
connection stays up).

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
