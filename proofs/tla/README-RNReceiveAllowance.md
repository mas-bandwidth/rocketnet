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

Filled in from the bench run; see the run notes below.
