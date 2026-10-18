# Using rocketnet

Call `rocketnet_init` once before using transports and check its result. A second
init returns false without resetting the live library. Destroy all objects and
free RocketNet allocations before `rocketnet_term`; termination with live
allocations is refused and logged. Repeated termination is a no-op. Startup,
shutdown and global configuration must be serialized with application activity.
Set custom allocator callbacks before initialization, with no live allocations.
Call `rocketnet_init` first: a create before it —
`rocketnet_client_transport_create`, `rocketnet_server_transport_create` or
`rocketnet_frame_sync_create` — returns NULL and allocates nothing. A refused
`rocketnet_allocator` replacement (late, while transitioning, with live
allocations, or with either callback NULL) returns false; an accepted
replacement returns true. Every refused call logs one line and increments
`rocketnet_api_misuse_count`, the process-wide count of refused API misuse.

The public API is also NULL safe at the boundary. Every entry point checks each
pointer argument it would dereference once, at the top of the call: a NULL is a
no-op that returns the function's documented value (`0`, `NULL` or `false`; a
void function returns nothing), increments `rocketnet_api_misuse_count`, and in
a debug build fires `rocketnet_assert`. A NULL handle is refused before any
I/O, so no socket opens. The check is one predictable branch at the top, never
inside a loop and never on an internal call, so the send and receive hot paths
pay nothing when their pointers are valid.

Endpoint ports are decimal digits in `0..65535`; signs, whitespace, missing
digits and trailing text are rejected. IPv6 with a port uses `[address]:port`.
Bare IPv6 and `[address]` have port zero. A zero server bind port requests an
ephemeral port; read the actual port after creation when minting a token.

The authority for the API is [sdk/rocketnet.h](sdk/rocketnet.h), and it is worth
knowing how it is written before you go looking in it: **contracts are prose
blocks covering a surface** — the send path, the delivered batch, the config
table, the driver ladder — **not one comment per function.** So the rule for
reading it is to find the block above the *region* a function sits in, not the
line above the declaration. Several of the functions this file describes,
`rocketnet_server_transport_start_packet` among them, have no comment of their
own and are governed by a block further up.

Read the block for what it actually covers, though, rather than assuming it
covers everything below it. Some blocks are narrow: the one describing the driver
accessor is followed by `rocketnet_client_transport_client_index` and
`rocketnet_client_transport_disconnect`, which it says nothing about. Proximity
is a hint about where to look, not a claim of scope.

This file is the narrative the header cannot be: the shape of a frame, the
contracts that are sharp enough to cut, and the configuration system that decides
what a transport actually is at create.

Build and driver-matrix instructions live in [BUILDING.md](BUILDING.md); the wire
format lives in [STANDARD.md](STANDARD.md).

## The transport cycle

Both ends run the same four phases in order, at the application's chosen cadence:

    update( time )        // receive, decrypt, deliver; publish the batches
    ... drain ...         // read everything update delivered
    ... stage ...         // start_packet / finish_packet, or send_packet
    send_packets()        // flush everything staged

`update` and `send_packets` are main-thread. `start_packet`, `finish_packet` and
`abort_packet` may run on any number of game threads, strictly between them, and
the handoff from fill to flush must be a synchronizing operation — a join or a
barrier, never a wall-clock wait. `disconnect`, `disconnect_client` and `stop` are
main-thread too, and never while a fill is open.

Call `send_packets` after each joined fill phase. There is no required frame,
update, send or ACK rate. What a caller owns is the
guarantee, not a driver's eagerness: everything staged before `send_packets` is
sent by it, and nothing is guaranteed sent without it.

The `time` passed to `update` — and to `rocketnet_client_transport_create` — must
be finite and non-decreasing: a game clock in seconds, monotonic. A NaN, an
infinity or a step backwards is clamped to the last good time, counted once in
`rocketnet_api_misuse_count`, and asserted (`rocketnet_assert`) in a debug build;
a release build clamps and goes on rather than freezing a timeout or rewinding a
deadline. The library never reads a clock of its own on these paths, so a game
owns time and a test can hold it still.

Driver eagerness above that floor differs, and it is a *server*-side difference:
on the server the GSO rung flushes update's own staging — protocol responses,
keep-alives — inline in update, and so do the portable and msgx rungs, which
share that path; the XDP rung stages and sends nothing until `send_packets`
(the driver fork in `rocketnet_server_transport_update`). Code against the
guarantee, not against any of that. Calling `send_packets` again without newly staged packets is a no-op.

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
prevents is the former C# loopback smoke's first flake (#73): a reply staged from somewhere that
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

A request at or under `ROCKETNET_MAX_PAYLOAD_BYTES` (1416) is a real wire frame,
zero copies. A larger request — up to the configured maximum — routes through the
transport's arena and `finish_packet` splits it into type 9 fragments, one copy,
spent only where splitting makes it unavoidable. Either way the context's single
`packet_id` is the payload's, and a fragmented payload's id surfaces in the acked
batch only when every fragment has been acknowledged. A lost fragment loses the
whole payload. The transport never resends.

**Once `start_packet` has handed you a frame, a session sequence is spent.** It
is allocated after the call's validation but *before* the frame is acquired:
`rocketnet_server_transport_start_packet` calls
`rocketnet_server_protocol_start_payload`, whose atomic add on
`send_sequence_session` is the allocation. Nothing gives it back.

Not every NULL from `start_packet` spends one. The checks that run *before* the
allocation — a transport that is not RUNNING, a `request_bytes` outside the
configured range, a client that is not connected, a slot with no encryption
mapping — return NULL having spent nothing. A failure at the *acquisition* step,
after the allocation, spends one: a full arena (`SEND_ARENA_FULL`) or a full ring
(`SEND_RING_FULL`).

**A fragmented payload spends far more than one, and this is the number to
reason with.** `start_packet` spends one; then `finish_packet`, on the arena
route, reserves `fragment_count - 1` more in a block
(`rocketnet_server_protocol_start_fragments`, called from
`rocketnet_server_transport_finish_arena_packet`) — fragment 0 rides the sequence
`start_packet` already spent, and the rest come from that block. So a fragmented
payload occupies `fragment_count` consecutive sequences. At the default
16384-byte maximum payload over a 1413-byte fragment budget, `fragment_count` is
**12**.

That matters because of where the failures are. A `finish_packet` that returns
zero *after* the block is reserved — a seal failure inside the fragment loop —
withdraws the whole reservation on the portable and GSO drivers (the
`wire_bytes == 0` branch of that function's seal loop), so **one such failure
burns all 14 at the default**, not one. On XDP, fragments already committed to the
asynchronous staging stay committed, and the burn is the remainder. Anything that
fails *before* the block is reserved — the plan, the frame reservations, the
block reservation itself — burns only the single sequence `start_packet` spent.

So the ways to burn, in rough order of how much they cost:

- a zero-returning `finish_packet` on a **fragmented** payload, after the block
  is reserved: up to `fragment_count` sequences;
- a `start_packet` that fails to acquire a frame, an `abort_packet`, or a
  zero-returning `finish_packet` on an unfragmented payload: one each.

The symbols above are the server's. **The client has the same shape** —
`rocketnet_client_protocol_start_fragments` reserves the same
`fragment_count - 1` block and a seal failure withdraws the same whole
reservation — so the counts apply to both ends. The client has no staged driver,
so it has no partial-withdrawal case.

The receiver's replay window tolerates the gaps, but **receive-side loss
statistics cannot distinguish them from real loss** — that figure is computed
from gaps in the session sequence, so each burned sequence reads as one lost
packet in a link that dropped nothing. Calibrate with the counts above: a run of
failed fragmented sends shows up as loss at 14x the rate an unfragmented one
would, at the default payload size.

Where a burn does and does not show up is worth being exact about, because the
two surfaces disagree.

**In the loss statistics:** the client publishes a receive-side `packet_loss`,
the server's per-client `packet_loss` is send-side, and neither end publishes a
receive-side figure for the server. So server-to-client burns appear as phantom
loss in `rocketnet_client_transport_stats`, and **client-to-server burns appear in
no loss statistic at either end.**

**In the counters, which is where to actually look:** a burn caused by *ring or
arena pressure* is counted at the end that burned it — `SEND_RING_FULL` and
`SEND_ARENA_FULL`, on both the client and the server, through
`rocketnet_client_transport_counters` / `rocketnet_server_transport_counters`. So
a client-to-server burn under pressure is invisible as *loss* but plainly visible
as *pressure*, and that is the number to reach for.

Pressure counts on more paths than the obvious one: a `finish_packet` on the
arena route that cannot reserve a wire frame for every fragment also increments
`SEND_RING_FULL`, from inside the reservation loop
(`rocketnet_server_reserve_fill_frame`). So a zero-returning
`finish_packet` is *not* uniformly counter-silent — it is silent on the
unfragmented wire route and counted on the arena route when the ring is what
refused it.

What is genuinely counter-silent: `abort_packet`, and a `finish_packet` refused
for a reason that is not pressure — a payload out of range, a connection gone, a
seal failure. Those you have to know you caused.

`send_packet` (the convenience form) exists on both ends. On the client it
flushes the ring inline, which makes it main-thread only and never valid during
an open fill; it is the simple form, not a fill-phase participant.

## Counters

Every transport owns a counters block, read through
`rocketnet_client_transport_counters` and `rocketnet_server_transport_counters`
and indexed by the `ROCKETNET_COUNTER_*` enum in `sdk/rocketnet_counters.h`.
Counters are monotonic and never reset; rates are differences of snapshots.
`rocketnet_num_counters`, `rocketnet_counter_name` and `rocketnet_counter_value`
read the list and a block without assuming its length.

**Every counter has a class**, what a clean run shows of it, and
`rocketnet_counter_class` returns it (`rocketnet_counter_class_name` spells it:
`"refusal"`, `"expected"`, `"path_loss"`, `"arrival"`):

- `refusal` reads 0 on a clean path at any round trip. A packet refused for a
  reason is a refusal, and so is a resource exhaustion (`SEND_RING_FULL`,
  `MESSAGE_SEND_STORE_FULL`, `SEND_HISTORY_OVERFLOW`).
- `expected` moves on a clean path by design: success outcomes, lifecycle
  counters, and side counters such as `ACK_UNKNOWN_SEQUENCE`,
  `CONNECTION_REQUEST_NO_COOKIE` (the first leg of the cookie exchange),
  `DISCONNECT_NO_MAPPING` (the redundant disconnect burst), the
  `ALREADY_CONNECTED` duplicates a round trip above 100 ms leaves, and the
  session-end race (a session that ends with the other end's packets in flight
  sends them to an end that is gone).
- `path_loss` moves only when the path loses or reorders packets
  (`MESSAGE_RESENT`, `FRAGMENT_PAYLOAD_ABANDONED`).
- `arrival` is a `RECEIVED` counter: a packet of the type arrived.

A load gate or a soak computes its zero-list from the classes: after a clean
run every `refusal` reads 0, and a run that injects a fault lists the refusals
the fault explains. No script keeps its own list of counter names.

**Every packet type is conserved.** A `RECEIVED` counter is incremented once on
entry, and then exactly one outcome counter is. `rocketnet_counters_conserved`
checks that for every type at once: it returns 1 when each `RECEIVED` counter
equals the sum of the outcomes that name it as their family, and otherwise 0,
with the first unbalanced family written to the report buffer as
`<RECEIVED name> received=<n> outcomes=<m>`. Call it on a block read at a
quiescent point, on the thread that owns it (after an update returns, when every
arrival has taken its outcome). Side counters (`ACK_UNKNOWN_SEQUENCE`, the
fragment payload counters) and the packet entry's `PACKETS_RECEIVED` sit outside
every family. An outcome belongs to one family only: the connection response
ladder's internal error is `CONNECTION_RESPONSE_INTERNAL_ERROR`, not the request
ladder's.

```c
char report[256];
if ( !rocketnet_counters_conserved( rocketnet_server_transport_counters( server ), report, sizeof(report) ) )
    printf( "counters out of balance: %s\n", report );
```

## Messages

The message objects sit on the transports and carry reliable ordered and
unreliable messages, each a pointer and a length. One client object or one
server object per endpoint, created once; nothing is allocated after create.

    struct rocketnet_message_server_t * server = rocketnet_message_server_create( NULL, "0.0.0.0:40000", &server_config, NULL, NULL, 0, 0 );

    // each frame, on the main thread
    rocketnet_message_server_update( server, time );        // ROCKETNET_ERROR_MESSAGE_NOT_RELEASED if a view was kept

    for ( int client_index = 0; client_index < max_clients; client_index++ )
    {
        const uint8_t * data;
        int bytes;
        while ( rocketnet_message_server_receive( server, client_index, channel, &data, &bytes ) )
        {
            handle( data, bytes );                           // copy what is kept past the next update
            rocketnet_message_server_release( server, client_index, data );
        }
    }

    uint8_t * p = rocketnet_message_server_start( server, client_index, channel, ROCKETNET_RELIABLE_ORDERED, max_bytes );
    if ( p )
    {
        int written = write_message( p, max_bytes );         // written once, in place
        rocketnet_message_server_finish( server, client_index, written );
    }

    rocketnet_message_server_send_packets( server );

The client is the same with no client index, and takes the connect token at
create but no time: its connect attempt starts at its first update, at
whatever the game's clock reads then. Every received message is released
before the next update. `start` returning NULL for a connected client with a valid
channel and size means its memory was exhausted: that connection has ended
with `ROCKETNET_MESSAGE_ERROR_MESSAGE_MEMORY_EXHAUSTED`, readable from
`rocketnet_message_*_disconnect_reason`. An unreliable message arrives on
channel 0 and is at most 1411 bytes. Each connection's send bandwidth
budget is in kbps, configured by the message config's `client_send_kbps`
(default 1000) and `server_send_kbps` (default 10000 per server slot). At
create, 0 takes that field and a positive value overrides it; a value of 0 or
lower disables the budget, which is not recommended. `rocketnet_message_*_set_send_kbps`
changes it at runtime under the same rule. The environment variables
`ROCKETNET_MESSAGE_CLIENT_SEND_KBPS` and `ROCKETNET_MESSAGE_SERVER_SEND_KBPS`
override the field and the create argument, logged at create. The design is
docs/CHANNELS.md, section 11.5 for what is built.

## Connection quality

`rocketnet_client_transport_stats` and
`rocketnet_server_transport_client_stats` report rtt and jitter in milliseconds,
packet loss as a percentage, and trailing rates.

Both those calls, and `rocketnet_server_transport_stats`, also report
cumulative wire totals: `packets_sent`, `packets_received`, `bytes_sent` and
`bytes_received`, in datagrams and bytes including the IP and UDP headers the
send budget counts. The client's totals cover its current connection; the
server's per-client totals cover the slot's current connection and reset when
a new connection takes the slot; the server's own totals cover the transport's
lifetime.

**Read the rtt number for what it is: a floor, not a current reading.** Sampling
is continuous — since #84 closed, the first ack block acknowledging each sent
sequence yields a sample — but the published figure is not the latest sample. It
is *the minimum over the whole rtt history*, which is the header's own model
(`rocketnet_ack.h`, above `rocketnet_ack_endpoint_update_stats`: "rtt is the
minimum over the rtt history, jitter the mean deviation from it"). The history is
`ROCKETNET_RTT_HISTORY_SIZE` = 2048 entries indexed by **sent sequence**, so the
window is a packet count, not a duration, and how long it spans depends entirely
on your send rate: roughly 200 seconds at 10 sends per second, roughly 34 at 60.
Payload sends consume entries alongside keep-alives, so a busy connection recycles
the ring faster than a quiet one.

The consequence is the one that bites: **this figure cannot track latency
upward.** It falls immediately when the path gets faster, and it rises only once
every lower sample has been evicted from the ring — up to the whole window later.
A congestion event that doubles real latency may not move it at all while any
pre-congestion sample survives. So it is a good "how fast can this path be" and a
bad input to a timeout, an alert, or anything that needs to notice degradation.
For that, watch jitter and loss — unlike the rtt floor, both *rise* when
conditions worsen — or track your own round trips from the acked batch. Of the
two jitter figures the client's is the more responsive; see below.

(Earlier notes describing rtt as frozen at its handshake value are stale in a
different way: that was the state before #84, when there was no ack sampling at
all.)

The **handshake seed is client-only**. The client falls back to its handshake
round-trip estimate while no ack sample exists yet, so its rtt is useful
immediately. The server has no such fallback: a client's rtt reads **0.0 until
that slot's first ack sample arrives**. Treat a server-side zero as "not measured
yet", not as a zero-millisecond link.

**Jitter is not the same measurement on the two ends**, so the two figures are
not comparable even though both are milliseconds. The client's jitter is
**inter-arrival** jitter over the session sequence — a property of packet arrival
spacing, kept as an exponential moving average that moves 1% toward each new
sample, so it tracks a change within roughly a hundred packets. The server's
per-client jitter is the **mean deviation of its rtt samples above their
minimum** — a property of its round trips, computed off the same history as the
rtt floor above and therefore averaged over that whole ring.

Both *rise* when conditions worsen, which is what makes them usable where the rtt
floor is not; they simply respond on different timescales and measure different
things. The client's is the one to watch for a fast signal — with one caveat that
matters precisely when you are watching: it updates **only on in-order arrivals**,
so while packets are being lost it stops moving rather than climbing. Read it
alongside `packet_loss`, which is what responds to that case. A client jitter of
5 ms and a server jitter of 5 ms are not the same statement about the connection.

Loss is asymmetric too, and the difference matters when you compare the two ends:
the client's `packet_loss` is **receive-side**, from gaps in the session sequence
— the figure the burned-sequence rule above perturbs. The server's per-client
`packet_loss` is **send-side**: the unacknowledged fraction of the older half of
its retained sent history. It is an estimate, not proof of delivery loss: delayed
ACKs can raise it. Neither end reports both. No rate or timeout policy should
treat this estimate as proof of loss.

Whole-transport packet and bandwidth rates use time-based buckets with about
4 ms boundary resolution. Calling update thousands of times within a second,
or repeatedly at one timestamp, does not shorten that history.

Delivery notification is the primitive underneath all of it:
`acked_packets` returns the packet ids newly acknowledged by the last update —
valid until the next update, like everything else here. The transport informs and
never resends; reliability policy is yours to build on top. Keep-alive, disconnect
and standalone-ack sequences carry no packet id and never appear.

ACK/replay history has a **4,096-sequence minimum and default**. A build may choose
up to 32,768 entries with `-DROCKETNET_HISTORY_PACKETS=...` (a multiple of 64).
This is a memory budget, not a packets-per-frame, packets-per-second or ACK-rate
requirement. Records do not expire with elapsed time. Returned ACK batches cover
the entire retained window for every configured client. Sending beyond that
window overwrites older delivery history and continues transmitting. An
overwritten record that was never acknowledged is counted by what the sender
knows of it. `SEND_HISTORY_LOST` counts a packet known lost on the path: the
peer has acknowledged a sequence at least `ROCKETNET_LOSS_PACKET_THRESHOLD` (3,
the packet threshold of RFC 9002 section 6.1.1) above it. It is an outcome,
and a lossy path raises it. `SEND_HISTORY_OVERFLOW` counts a record whose fate
is unknown (the peer has been silent, or the window is too small for the path)
and a writer that fell behind the retained window. It is a refusal of history
and reads zero on a healthy path, however long the connection runs and however
long the round trip is. Missing a notification therefore does not prove that a
payload was lost. The remote peer's receive-history depth and the ACK block's
255-range encoding bound also limit what it can describe after extreme loss.

Packet contexts are validated against private leases before payload access,
finish and abort. A copied context can be consumed once; changed fields, stale
phases and wrong-owner contexts are refused. Native callers can use
`rocketnet_*_transport_packet_payload` for a checked pointer/capacity pair.
An acquired pointer/span expires at finish, abort, update, flush or destruction.
Do not race those operations or retain their memory views afterward. The client
has metadata for 512 acquisitions per fill, the server for 65,536; driver rings
and the 16-slot fragmentation arena may exhaust first. `SEND_LEASE_FULL` reports
metadata exhaustion. These are staging budgets, independent of elapsed time.

## Configuration

One X-macro table per transport in `rocketnet.h` derives the struct, the defaults,
the environment variable names, the parse and the validation. For everything in
the table, there is one declaration and the rest is generated from it.

The server field `handshake_deadline_seconds` defaults to 5 seconds and accepts
1 to 60. It fixes the half-open mapping deadline when the challenge is created;
requests never refresh it, and connecting discharges it. Set it in the server
config or with `ROCKETNET_SERVER_HANDSHAKE_DEADLINE_SECONDS`. It is independent
of the connect token's connected idle timeout.

**The table is not the whole environment surface**, and the difference matters
because only the table's variables get the precedence, logging and validation
described below. Reading every `getenv` in `sdk/`, these sit outside it:

- `ROCKETNET_PUMP` — see below, it has the sharpest edge;
- `ROCKETNET_LOG_LEVEL`, read once at process init;
- `ROCKETNET_GATEWAY_ETHERNET_ADDRESS`, read by the XDP driver;
- `ROCKETNET_SERVER_NUM_QUEUES` — the one surviving deprecated alias, of the
  server table's `ROCKETNET_SERVER_NUM_XDP_QUEUES`. It is honored only where
  its successor is unset, and every use is logged naming the successor.

Every config field has **one name per endpoint**: the server table reads
`ROCKETNET_SERVER_<FIELD>` and the client table reads `ROCKETNET_CLIENT_<FIELD>`,
so a server and a client in one process never share a variable.
`ROCKETNET_SERVER_NUM_OS_CPUS` and `ROCKETNET_SERVER_PUBLIC_ADDRESS` are the
canonical names of their fields. The old process-wide spelling
`ROCKETNET_<FIELD>` is **no longer applied**: a variable set under it is
refused with one error naming its per-endpoint replacement, once per create,
so an operator is never silently ignored.

**`ROCKETNET_PUMP=portable` refuses the Darwin msgx candidate**, including
an explicit msgx pin. The deprecated switch retains its original pump-disable
meaning: `auto` may continue to portable, while `driver = "msgx"` fails loudly
because its only candidate refused. It never silently changes a pin to portable.
Unset the legacy variable to allow msgx, or use `ROCKETNET_SERVER_TRANSPORT` /
`ROCKETNET_CLIENT_TRANSPORT` `=portable` to request portable through the current
driver selector. The refusal log names both the old switch and its successor.

**Precedence is default < struct < env, evaluated per endpoint.** The struct you
pass is the middle layer, and the environment overlays it.

Every environment override is logged loudly,
naming the variable, the new value and the value it displaced — with secret fields
redacted on both sides, so a key's encoding never reaches a log line. Validation
**refuses and never clamps**: an out-of-range value fails the create with an error
naming the field and its range.

Environment names are mechanical: `ROCKETNET_` plus the endpoint plus the field
name uppercased. On the server `max_clients` is `ROCKETNET_SERVER_MAX_CLIENTS`,
`driver` is `ROCKETNET_SERVER_TRANSPORT`, `public_address` is
`ROCKETNET_SERVER_PUBLIC_ADDRESS`; on the client `driver` is
`ROCKETNET_CLIENT_TRANSPORT`. A field's old process-wide spelling
(`ROCKETNET_MAX_CLIENTS`, `ROCKETNET_DRIVER`, `ROCKETNET_PUBLIC_ADDRESS`) is
refused aloud with the per-endpoint name to use instead. Byte fields (keys,
filter value) are base64. An environment variable set to the empty string is
treated as unset, with a log line saying so.

**Passing `NULL` for the config is the default shape** — and on the two ends that
means different things:

- **Client:** `rocketnet_client_transport_create( context, token, NULL, time )` is
  the ordinary path. The token names the server and carries the keys; the config
  only carries what the token does not.

- **Server:** `rocketnet_server_transport_create( context, address, NULL )`
  gets the default shape, which has **no keys** — and a server with no keys is
  refused, never defaulted, because default keys are how every deployment of a
  protocol ends up sharing one identity. So a NULL config succeeds on the server
  only when the environment supplies all three keys. For development, fill
  `rocketnet_server_transport_dev_config`, which installs the published
  development keys loudly. For production, fill the struct or set
  `ROCKETNET_SERVER_SERVER_PUBLIC_KEY`, `ROCKETNET_SERVER_SERVER_SECRET_KEY`
  and `ROCKETNET_SERVER_BACKEND_PUBLIC_KEY`.

`config.public_address`, when set, displaces the address parameter passed to
`create` — so the address the driver ladder verifies, the address the driver binds
and the address connect tokens validate against can never diverge. The struct form
is per create; the **environment** form is scoped to the first server create in
the process and then consumed, loudly, so a deployed binary's one server gets its
redirect while a multi-transport process is not wholesale redirected by one ambient
variable. It is consumed only after a create that fully succeeds.

**Operator memory budgets** (`ROCKETNET_SERVER_MEMORY_BUDGET_BYTES` and
`ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES`, the server and client tables'
`memory_budget_bytes` field) cap a transport's pre-allocation memory plan. At
create, each driver candidate's plan is compared to the budget before anything
allocates: a candidate whose plan exceeds the budget is refused, logged with the
plan and the budget, and the ladder falls to the next candidate exactly as a
refusing driver does. A budget that no candidate fits refuses the create before
the first allocation. Unset or `0` means no budget. The plan is the allocator
footprint from the constructor memory plan functions (`required_memory`), not a
host RAM limit; it excludes OS stacks, kernel queues, loader and application
memory. This lets an operator bound one cross-field allocation class from the
environment, without code or a rebuild.

## Driver selection

Selection is **runtime**, not compile time (#106): one build carries every driver
its platform has, and the transport walks an ordered candidate list at create,
left to right, **first grant wins**. The list rides `config.driver` or
`ROCKETNET_SERVER_TRANSPORT` (server) / `ROCKETNET_CLIENT_TRANSPORT` (client), under
the usual precedence.

    driver = "auto"            the platform's recommended order for this endpoint
    driver = "gso"             a one-element list is a PIN: that driver or failure
    driver = "gso,portable"    exactly these, exactly this order

The recommended orders — what `auto` expands to — are:

|          | server               | client            |
|----------|----------------------|-------------------|
| Linux    | xdp, gso, portable   | portable          |
| macOS    | msgx, portable       | msgx, portable    |
| iOS      | portable             | portable          |
| Windows  | portable, iocp       | portable          |
| BSDs     | portable             | portable          |

The new Windows IOCP server driver is explicitly selectable with `"iocp"`.
Its automatic preference remains behind portable during validation. See
[Windows IOCP server](BUILDING.md#windows-iocp-server) for worker configuration,
completion ownership and the scope of its checks.

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
visible at the log level deployed binaries actually run at.

So `rocketnet_server_transport_port` gives you a real port by one of two routes,
and it is worth knowing which: either the public address carried a port, and that
is the port — including on an XDP grant, which binds what it was given — or it
carried zero, in which case the rule above guarantees the driver that granted was
one that discovers.
