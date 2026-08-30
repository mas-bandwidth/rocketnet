# Using rocketnet

The authority for the API is [sdk/rocketnet.h](sdk/rocketnet.h) — every function
carries its contract above it. This file is the narrative the header cannot be:
the shape of a frame, the contracts that are sharp enough to cut, and the
configuration system that decides what a transport actually is at create.

Build and driver-matrix instructions live in [BUILDING.md](BUILDING.md); the wire
format lives in [STANDARD.md](STANDARD.md).

## The frame

Both ends run the same four phases, in this order, once per frame:

    update( time )        // receive, decrypt, deliver; publish the batches
    ... drain ...         // read everything update delivered
    ... stage ...         // start_packet / finish_packet, or send_packet
    send_packets()        // flush everything staged

`update` and `send_packets` are main-thread. `start_packet`, `finish_packet` and
`abort_packet` may run on any number of game threads, strictly between them, and
the handoff from fill to flush must be a synchronizing operation — a join or a
barrier, never a wall-clock wait. `disconnect`, `disconnect_client` and `stop` are
main-thread too, and never while a fill is open.

`send_packets` is required once per frame, after update. What a caller owns is the
guarantee, not a driver's eagerness: everything staged before `send_packets` is
sent by it, and nothing is guaranteed sent without it. Every driver except XDP —
portable, msgx and GSO — happens to flush update's own staging (protocol
responses, keep-alives) inline in update; the XDP driver sends nothing until
`send_packets`. Code against the guarantee. Calling `send_packets` twice in a
frame is a no-op.

## Consuming what arrived

This is the contract that has actually bitten, so it gets the most words.

**The delivered batch belongs to the update that published it.** Every update
resets the batch to empty and refills it, and the payload pointers alias storage
the library owns and the next update overwrites in place. Two consequences, and
the second is the one that hurts:

1. **Copy anything you keep.** A pointer from
   `rocketnet_server_transport_payload` or `rocketnet_client_transport_payload`
   is valid until the next update and not one instruction longer. Bindings that
   hand out a span or a `ReadOnlySpan<byte>` are handing out that same window.

2. **Drain every update.** The batch is replaced, never appended to. A payload
   delivered by an update nobody drained is gone at the API surface — the
   counters still counted it, but no accessor will ever show it again. There is
   no backlog to catch up on.

**So replies stage after the drain, and in the loop that will drain the answer.**
That is the shape of the pump above, and it is not stylistic. The failure it
prevents is the C# smoke's first flake (#73): a reply staged from somewhere that
pumps its own updates — a helper, a nested wait loop, a second pump — puts a
payload in flight toward a peer while nobody is positioned to drain the response,
and the response is delivered into an update whose batch is then discarded. It
does not look like loss. The counters show the payload delivered; the consumer
simply never sees it, and it is intermittent, because it depends on which update
carries the answer.

The rule in one line: **everything in flight toward a peer must be drained by the
loop that expects it.**

Both ends expose the batch two ways: indexed (`num_payloads` + `payload`) and as
parallel arrays in one call (`payloads`). The **client** has a third,
`rocketnet_client_transport_receive_packet`, a copying cursor that pops the next
payload into a buffer you own and is therefore the one form whose result outlives
the next update. There is no server equivalent; a server drains the batch and
copies what it keeps. The cursor refuses a payload larger than the buffer you
gave it whole rather than truncating it, counts `RECEIVE_CURSOR_TOO_SMALL`, and
moves on to the next payload that fits; size by
`rocketnet_client_transport_max_payload_bytes` to never refuse.

Reassembled fragmented payloads take their place in the batch in arrival
position, at the moment they complete, up to the configured maximum payload —
size any buffer you copy into by that configuration, not by the wire's ceiling.

## Sending

`start_packet` hands back a 16-aligned frame; write the payload at
`frame + payload_index`; `finish_packet` seals it in place on the calling thread
and stages it. Nothing is copied at any step — the packet is encrypted where it
was written. `context.payload_capacity` is the frame's real capacity, never the
request; write up to it and `finish_packet` routes on what you actually wrote.

A request at or under `ROCKETNET_MAX_PAYLOAD_BYTES` (1192) is a real wire frame,
zero copies. A larger request — up to the configured maximum — routes through the
transport's arena and `finish_packet` splits it into type 9 fragments, one copy,
spent only where splitting makes it unavoidable. Either way the context's single
`packet_id` is the payload's, and a fragmented payload's id surfaces in the acked
batch only when every fragment has been acknowledged. A lost fragment loses the
whole payload. The transport never resends.

**A started packet burns a session sequence, whatever happens next.**
`start_packet` allocates the sequence as its *first* act, before it tries to
acquire a frame at all, and nothing gives it back. Three ways to burn one:

- `abort_packet`;
- a `finish_packet` that returns zero — payload out of range, or the connection
  gone;
- and the one that dominates under pressure: **a `start_packet` that returns
  NULL.** The sequence is already spent by then, so a full send arena
  (`SEND_ARENA_FULL`) or a full ring (`SEND_RING_FULL`) burns one on every
  refused acquisition. Aborts cluster; ring-full does not have to cluster to
  outnumber them.

The receiver's replay window tolerates the gaps, but **receive-side loss
statistics cannot distinguish them from real loss** — that figure is computed
from gaps in the session sequence, so a burned sequence reads as one lost packet
in a link that dropped nothing.

Which end actually sees it is not symmetric, and this is the part worth getting
right: only the **client** publishes a receive-side loss figure. The server's
per-client `packet_loss` is send-side, so **a client's burned sequences surface
nowhere in this API** — they are visible to the server only through its counters.
Server-to-client burns are the ones that show up as phantom loss, in
`rocketnet_client_transport_stats`. Read either figure with the send-side
counters beside it.

`send_packet` (the convenience form) exists on both ends. On the client it
flushes the ring inline, which makes it main-thread only and never valid during
an open fill; it is the simple form, not a fill-phase participant.

## Connection quality

`rocketnet_client_transport_stats` and
`rocketnet_server_transport_client_stats` report rtt and jitter in milliseconds,
packet loss as a percentage, and trailing rates. Since #84 closed, **rtt is
live** on both ends: it is refreshed by the ack blocks every session packet
carries, the first block acknowledging each sent sequence yielding a sample.
(Earlier notes describing rtt as frozen at its handshake value are stale; that was
the state before #84.)

The **handshake seed is client-only**. The client falls back to its handshake
round-trip estimate while no ack sample exists yet, so its rtt is useful
immediately. The server has no such fallback: a client's rtt reads **0.0 until
that slot's first ack sample arrives**. Treat a server-side zero as "not measured
yet", not as a zero-millisecond link.

Loss is asymmetric too, and the difference matters when you compare the two ends:
the client's `packet_loss` is **receive-side**, from gaps in the session sequence
— the figure the burned-sequence rule above perturbs. The server's per-client
`packet_loss` is **send-side**: the percentage of its own sent packets never
acknowledged. Neither end reports both.

Delivery notification is the primitive underneath all of it:
`acked_packets` returns the packet ids newly acknowledged by the last update —
valid until the next update, like everything else here. The transport informs and
never resends; reliability policy is yours to build on top. Keep-alive, disconnect
and standalone-ack sequences carry no packet id and never appear.

## Configuration

One X-macro table per transport in `rocketnet.h` derives the struct, the defaults,
the environment variable names, the parse and the validation. There is one
declaration; everything else is generated from it.

**Precedence is default < struct < env.** The struct you pass is the middle layer,
and the environment overlays it. Every environment override is logged loudly,
naming the variable, the new value and the value it displaced — with secret fields
redacted on both sides, so a key's encoding never reaches a log line. Validation
**refuses and never clamps**: an out-of-range value fails the create with an error
naming the field and its range.

Environment names are mechanical: `ROCKETNET_` plus the field name uppercased.
`max_clients` is `ROCKETNET_MAX_CLIENTS`, `driver` is `ROCKETNET_DRIVER`,
`public_address` is `ROCKETNET_PUBLIC_ADDRESS`. Byte fields (keys, filter value)
are base64. An environment variable set to the empty string is treated as unset,
with a log line saying so.

**Passing `NULL` for the config is the default shape** — and on the two ends that
means different things:

- **Client:** `rocketnet_client_transport_create( context, token, NULL, time )` is
  the ordinary path. The token names the server and carries the keys; the config
  only carries what the token does not.

- **Server:** `rocketnet_server_transport_create( context, address, NULL, time )`
  gets the default shape, which has **no keys** — and a server with no keys is
  refused, never defaulted, because default keys are how every deployment of a
  protocol ends up sharing one identity. So a NULL config succeeds on the server
  only when the environment supplies all three keys. For development, fill
  `rocketnet_server_transport_dev_config`, which installs the published
  development keys loudly. For production, fill the struct or set
  `ROCKETNET_SERVER_PUBLIC_KEY`, `ROCKETNET_SERVER_SECRET_KEY` and
  `ROCKETNET_BACKEND_PUBLIC_KEY`.

`config.public_address`, when set, displaces the address parameter passed to
`create` — so the address the driver ladder verifies, the address the driver binds
and the address connect tokens validate against can never diverge. The struct form
is per create; the **environment** form is scoped to the first server create in
the process and then consumed, loudly, so a deployed binary's one server gets its
redirect while a multi-transport process is not wholesale redirected by one ambient
variable. It is consumed only after a create that fully succeeds.

## Driver selection

Selection is **runtime**, not compile time (#106): one build carries every driver
its platform has, and the transport walks an ordered candidate list at create,
left to right, **first grant wins**. The list rides `config.driver` or
`ROCKETNET_DRIVER`, under the usual precedence.

    driver = "auto"            the platform's recommended order for this endpoint
    driver = "gso"             a one-element list is a PIN: that driver or failure
    driver = "gso,portable"    exactly these, exactly this order

The recommended orders — what `auto` expands to — are:

|          | server               | client            |
|----------|----------------------|-------------------|
| Linux    | xdp, gso, portable   | portable          |
| macOS    | msgx, portable       | msgx, portable    |
| iOS      | portable             | portable          |
| Windows  | portable             | portable          |
| BSDs     | portable             | portable          |

Four rules, and the distinctions between them are deliberate:

- **An explicit list is exactly that list.** Nothing is appended. A one-element
  list is a pin — that driver or a loud failure, never a silent fall that would
  mask a provisioning error.
- **A known name that this endpoint cannot use here is skipped**, with one soft
  log line: `gso` on a client, `xdp` on macOS. One fleet-wide value deploys
  everywhere. If *every* name in the list is skipped, the endpoint falls to its own
  auto order, loudly, rather than bricking an endpoint the value was not addressing.
- **A name in no transport set anywhere fails the parse**, loudly, listing the
  valid names, before anything is allocated. `protable` is a typo, not a silent
  fall to the floor; so is `GSO`, since names match exactly.
- **A rung that refuses is logged and the ladder moves on.** Refusal reasons are
  per driver: missing privilege or an unprovisioned machine for xdp, a queue count
  past its own geometry for gso. Per-driver bounds are refused, never clamped.

The **granted** driver is what matters and what is reported —
`rocketnet_server_transport_driver` / `rocketnet_client_transport_driver`, plus a
create log line naming the grant and listing **which** candidates refused ahead of
it. The *reasons* are not in that line: each rung logs its own refusal as it
happens, so diagnosing a fall means reading the warnings above the grant line, not
the grant line alone. Performance claims attach to the granted tier, never to the
requested one.

### Port zero and the driver that cannot discover

A public address carrying port zero asks the kernel for an ephemeral port; the
granted port is adopted into the advertised address — the one connect tokens are
minted for and validated against — and `rocketnet_server_transport_port` reads it
back. Tests and single-box tools should use it instead of hardcoding a port and
racing every other process on the machine.

Discovery is a **per-driver property**. The portable, msgx and GSO drivers bind and
read back what they were granted. The XDP driver binds the port it is given and
discovers nothing, so a port-zero address means nothing to it. Since #109 the
ladder refuses that combination at create rather than documenting it: a rung that
cannot discover refuses a port-zero address before it binds anything, naming the
rule. It is an ordinary rung refusal — `auto` falls past it to a driver that does
discover, logging a warning — and only a **pin** on a non-discovering driver
refuses the create outright, where the rule is restated as an **error** so it is
visible at the log level deployed binaries actually run at. Consequently
`rocketnet_server_transport_port` never returns zero on a transport that was
created.
