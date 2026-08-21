# RocketNet

**The name is RocketNet, one word.** Decided 2026-08-21: RocketWerkz is the
official sponsor and exercised its naming right. Verbatim quotes throughout this
file preserve whatever spelling was used in the moment and should not be
"corrected" — the record stays the record.

The transport foundation of the family: the layer that moves bytes, keeps
connections, and makes delivery reliable, at the highest IO rate each platform
allows. Everything else in the estate eventually runs over it.

Scope, ruled 2026-08-21, verbatim: "Rocket Net will be only the low level
transport library." It carries connection, reliability and IO, and it stops
there. Serialization, state replication and delta encoding are separate products
above it, and this library does not grow into them however convenient it would
be in the moment. The scope line is the design: a transport that stays a
transport can be replaced, ported and reasoned about, and one that accretes
features from the layer above becomes the yojimbo problem again.

And the equivalence, so nobody under-scopes it in the other direction, verbatim:
"It will handle the equivalent of what yojimbo does, but it will not expose
messages with serialize methods." — "that's the byte buffer thing." The
FUNCTIONALITY of the predecessor stays. Client and server, connection, reliable
and unreliable delivery, ordering, fragmentation: everything a game needs to get
payloads across intact. What goes is the INTERFACE. No message classes carrying
serialize methods, no message factories, no serialization vocabulary anywhere in
the API. A caller hands over bytes and receives bytes, and what produced those
bytes is none of the transport's business.

Nothing is built yet. This file is the founding record, written 2026-08-21 from
Glenn's own words so that a session arriving here cold knows what this is before
it writes a line.

## What it is, in his words

"the component that Dean Hall wants will be called 'Rocket Net'"

"It is going to be a merge of the 'flow' library and bits of netcode, reliable,
yojimbo, into a C# and C++ native library with AF_XDP support, extended to
windows completion ports and mac whatever is equivalent so they have max IO."

"It is the low-level foundation of space game pulled out and productized. it
will become open source AGPL with the MBSL commercial, and they will get to name
the library and be sponsors and pay no license fees from that point on."

"We will eventually make delta and schema and all the other things build on
Rocket Net."

"'flow' will become Rocket Net."

"it's the next generation replacement for the netcode/reliable/yojimbo combo."

"all-in-one. multi-language."

"simple (byte buffers only... no hardcore weird serialize messages like in
yojimbo)."

## Where it sits

Read the family upward and every layer has exactly one narrow contract:

- **Rocket Net** — bytes to the wire and back. Connection, reliability,
  acknowledgement, and maximum IO per platform.
- **serialize** — values to bits. Ported to C, Go, Rust, C# and JS, standardized
  and conformance-gated.
- **delta** (proposed, under study) — state over time to the fewest bits,
  through serialize.
- **schema** and the delta-side compiler — declarations to code, for all of the
  above.

No layer reaches through another. Rocket Net does not know what a message means;
it moves byte buffers. That is the correction of yojimbo's central mistake,
where message serialization and transport were entangled, and it is the same
move the game's lifetime model makes elsewhere: narrow the interface until the
bad case cannot be expressed.

## Design constraints, taken as settled

- **Byte buffers only** at the API. No serialization concepts, no message types,
  no schema knowledge.
- **All-in-one.** One library replacing the netcode plus reliable plus yojimbo
  combination, not three that must be assembled.
- **Multi-language**, with C++ and C# native first because that is what the
  first sponsor needs.
- **Maximum IO per platform**, which means the platform backends are the
  substance of the work: AF_XDP on Linux, completion ports on Windows, and
  whatever macOS offers as its equivalent.
- **Provenance**: flow is the starting point and carries the AF_XDP work; bits of
  netcode, reliable and yojimbo come across; the space game is the proving
  ground because it already runs this code in production.

## Ownership, and why it is how maintenance happens

Mas Bandwidth LLC owns this library and maintains it, and the reason is
engineering rather than commerce. Code that is built, delivered and handed over
is forgotten by the people who understand it: the author moves on, and eighteen
months later the person who could fix a subtle packet-loss bug is gone. Holding
ownership keeps the maintainer in the loop and keeps the library alive, which is
the property that actually matters to anyone depending on it.

One design consequence follows and it binds from the first commit rather than
from whenever licensing is settled: anything this library REQUIRES that a
consumer cannot obtain — a proprietary dependency, a compiler extension, a
constant from somebody's game — has to be a parameter rather than a
requirement. That discipline is demanded twice over, once by generality across
projects and once by the licensing this will eventually carry.

## When work starts here

The estate's standing practice applies: commit as Rowan
(`git -c user.name="Rowan" -c user.email="rowan@mas-bandwidth.com"`), tooling in
Go or shell and never Python, and the family's quality bar is serialize — a
standard document that defines the contract, a conformance gate, cross-language
proof rather than per-port claims, adversarial tests that can fail where they
must, and portability legs including big-endian and 32-bit and MSVC.

The first honest questions to sketch, none of them answered yet: what the API
looks like when it is only byte buffers; how the platform backends divide behind
one contract without the contract leaking the fastest one's assumptions; what
belongs in the merge from each of the three predecessors and what should be left
behind; and how a multi-language library keeps one wire contract when its
performance work is necessarily per-platform native.

## First requirements, from the first sponsor

Glenn, 2026-08-21, verbatim: "Dean wants it to support the low-level transport
like unreliable messages (byte buffer), reliable messages (byte buffer, like
block messages in yoimbo), fragmentation and re-assembly, and another type of
message which is most recent only."

Four delivery disciplines, every payload an opaque byte buffer:

- **Unreliable.** Sent once, may be lost, no ordering guarantee owed.
- **Reliable.** Delivered eventually, in order, in the shape of yojimbo's block
  messages: arbitrary size, acknowledged, retransmitted until received.
- **Fragmentation and reassembly.** Payloads larger than a packet split on send
  and rebuild on receive.
- **Most recent only.** The newest payload supersedes every older one. Note this
  has two halves and both matter: on receive, an arrival older than what has
  already been delivered is discarded rather than delivered late; on send, a
  newer payload queued behind an unsent older one should replace it rather than
  queue after it, which turns a backlog into a single current value and saves
  the bandwidth outright. This is the discipline that fits anything whose value
  is its freshness — input, state, a heartbeat — and it is the one of the four
  yojimbo did not have.

Design questions these raise, unanswered and worth answering before code:

Whether fragmentation is available to the unreliable and most-recent-only
disciplines or only to the reliable one. It matters more than it looks: the
space game today fragments its own large baselines and resends them several
times because it has no better tool, and Glenn's own stated future idea is to
resend only the fragments that were not acknowledged. A transport offering
fragmented delivery with per-fragment acknowledgement subsumes that hand-rolled
scheme entirely and puts it where it belongs, which is one reason the same
library serves both the sponsor's needs and the estate's.

How the four disciplines are exposed without the API growing a vocabulary: four
channel kinds configured at connection setup is the obvious shape, but the
number and configuration of channels is exactly the kind of thing that grows
into a message system if nobody watches it.

What ordering means for the reliable discipline across multiple channels, and
whether head-of-line blocking between independent streams is acceptable or has
to be designed out.

## The performance shape

Glenn, 2026-08-21, verbatim: "and we need to get it as fast as possible, so
zero-copy where we can and using all the good stuff in flow around start packet,
..., finish packet to enable going wide and multithreaded."

This decides the API before anything else does. The byte buffer contract is NOT
"hand me your buffer and I will copy it" — it is claim, write, commit. A caller
asks the transport for space, writes its payload directly into the transport's
own memory, and finishes the packet. That is what makes zero-copy possible at
all, and it is also what lets the thing go wide, because each thread claims its
own slot and never contends for a shared assembly buffer. It matches what the
fast path underneath wants: on AF_XDP the claimed slot is a descriptor in the
transmit ring, and a copy into it would be pure waste.

Two consequences that need deciding rather than discovering:

The receive side has the same shape in reverse, which means a received payload
is a pointer into the transport's memory rather than a copy the caller owns.
That is a lifetime contract, and it must be stated in the API's own terms —
valid until what, exactly, and what happens to a caller who holds it too long.
The fastest possible answer and the safest possible answer are not the same
answer, and this is the place to choose deliberately rather than by accident.

Most-recent-only sends interact with claim-and-commit in a way worth thinking
through early: if a newer payload is meant to supersede an older one that has
been written into a claimed slot but not yet sent, then superseding means
releasing or rewriting that slot. Fine, but it is real bookkeeping, and a design
that assumes committed means immutable will have to be unpicked later.

## Two constraints that decide the API's form

Glenn, 2026-08-21, verbatim: "It needs to be callable from C#" and "Rocket Net
should have no *structure*. Anything that structures packets sits above, eg.
serialize library, schema, delta, 'compiler x'."

**Callable from C# means the public contract is a C ABI.** Not idiomatic C++
with templates and classes at the boundary, because none of that crosses to C#.
Opaque handles, plain functions, blittable structs, no exceptions escaping, and
a header a binding generator can read. The good news is that the claim, write,
commit shape survives the crossing intact: a claimed buffer arrives in C# as a
span over native memory, so zero-copy holds on both sides of the boundary rather
than degrading into a marshalled copy the moment C# touches it. The thing to
design against is per-call overhead, since a transition per packet is a real
cost in a hot loop, which argues for batch-shaped entry points wherever the API
would otherwise be called once per small thing.

**No structure means the payload is opaque, completely.** The transport has its
own protocol material — sequence numbers, acknowledgements, fragment indices —
and that is its own business, carried in its own header space. What it must
never have is an opinion about what the caller's bytes mean. The four delivery
disciplines are about DELIVERY, not content: a most-recent-only channel compares
sequence numbers it assigned itself and never looks inside. Everything that
gives bytes a shape lives above — serialize, schema, delta, and the compiler
whose name is still open.

That is also the test to apply to every future feature request here. If a
proposal requires the transport to understand the payload, it belongs in a layer
above, however convenient it would be to put it here.

## The scale target

Glenn, 2026-08-21, verbatim: "rocket net will be implemented so it scales up to
1000 players or beyond." — "using the zero copy stuff on linux, or equivalent on
windows, or mac." — "so io bound doesn't limit it."

The same thousand-player number drives the layer above, and the two are solving
different halves of it. The delta work removed the CPU and cache bottleneck by
encoding once and broadcasting rather than building a delta per client, which
took server cost from O(clients x objects) to O(objects). This layer removes the
IO bottleneck, so that having won on CPU the server is not then stopped by
syscalls and copies. Neither win is worth much without the other, and that is
the argument for both existing.

A first-order question the combination raises, worth settling early because it
shapes the API rather than the implementation: the broadcast model means the
SAME payload goes to many connections. A transport that can take one written
buffer and send it to N destinations is obviously the right primitive for that,
but per-connection encryption complicates it — if a shared payload has to be
encrypted separately for each of a thousand connections, the crypto cost becomes
the new bottleneck exactly where the copy cost was removed, and the win moves
rather than lands. So: is there a multi-destination send, what does it actually
share, and what does the security model allow to be shared? That question wants
measurement rather than intuition, and it wants asking before the API sets.

Settled the same day, verbatim: "yes, it needs broadcast support for messages."

So broadcast is a first-class requirement rather than an optimization to consider
later, and it crosses the four disciplines rather than sitting beside them: the
game's baselines and deltas are broadcast, and they are large, which means
broadcast has to compose with fragmentation and with reliability, not only with
the unreliable case.

The consequence that reaches furthest into the design is buffer lifetime. A
unicast payload lives until it is sent. A broadcast payload lives until every
recipient has acknowledged it or timed out, so one written buffer is referenced
by many per-connection delivery states, and retransmission goes to the subset
that has not acknowledged rather than to everyone. That is refcounting over the
claimed buffers, and it interacts directly with the claim, write, commit model:
commit no longer means the transport is finished with a buffer at a knowable
moment. Worth designing deliberately, because a zero-copy pool whose occupancy
depends on the slowest client is a pool that can be pinned by one bad connection
unless something bounds it.

What remains open from the question above is only the security half: how much of
a broadcast payload can genuinely be shared once each connection encrypts with
its own key, and whether the answer is per-connection framing over a shared
body, a cipher arrangement that shares the expensive part, or simply paying it.
That wants measurement, and it is the difference between broadcast being a large
win and being a bookkeeping exercise.

## Security

Glenn, 2026-08-21, verbatim: "it should be secure like netcode (see flow...)".

The netcode model, already implemented in flow, which is where to read it rather
than reinventing it: connect tokens issued by a backend and carrying short-lived
per-client keys, authenticated encryption on every packet, replay protection,
and a handshake that gives an attacker nothing to amplify. That model comes
across whole. This is not a place to be creative.

It also answers the broadcast question above, and the answer is worth stating
plainly because it is not the convenient one. Under per-client keys, a broadcast
payload cannot simply be encrypted once and sent to everyone: each connection's
packet is sealed with its own key, so the naive reading is that broadcast saves
the encoding and the assembly but pays the cryptography a thousand times over.

The established way out is hybrid broadcast encryption, which is old and
well-understood: seal the bulk payload once under an ephemeral key, then give
each recipient that ephemeral key wrapped under their own, so the per-recipient
cost is a small constant rather than the payload size. Variants differ in where
authentication sits and what each recipient can forge, and those differences are
exactly where broadcast schemes go wrong.

So the standing instruction for whoever picks this up: this is a real question
with real prior art, it is NOT a place to invent a construction, and any scheme
that shares bulk cryptography across recipients gets reviewed against
established constructions and by someone who does cryptography for a living
before it ships. Measure the naive path first — it may simply be affordable at
the target scale, and affordable and boring beats clever and unreviewed.

## Where to start

Glenn, 2026-08-21, verbatim: "basically, it should start with the flow library as
the starting point." — "but we should remove the multi-cast stuff and the network
next like stuff from flow." — "at least in v1 (it's not implemented anywhere)".

flow is the private repository mas-bandwidth/flow, with working checkouts on this
machine at ~/flow and ~/rowan-working/flow. It carries the AF_XDP work and the
netcode-equivalent security, and it is the body this library grows from rather
than a reference to imitate.

Glenn on what flow was reaching for, and where that lands: "flow was meant to be
this, plus also have network next-like multicast optimization over a relay
network and ddos protection." — "that's probably, definitely overkill for
rocketnet v1" — "but is OK for it to be in for later versions. rocketnet is an
all-in-one transport product."

So the removals are DEFERRALS, not rejections, and the distinction matters to
anyone planning: relay-network multicast optimization in the Network Next
lineage, and DDoS protection, are both on the roadmap because an all-in-one
transport product eventually owns those concerns. They come out of v1 because a
first version whose whole claim is simplicity cannot also carry a relay network,
and because neither is implemented anywhere today, so removing them costs
nothing now and forecloses nothing later.

Three things share vocabulary here and must not be confused. BROADCAST, required
in v1, is an application-level send of one payload to many connections, each
still receiving its own authenticated unicast packet. RELAY MULTICAST, deferred,
is sending once to a relay network that fans out, which is a bandwidth and
routing optimization at the infrastructure level. IP MULTICAST is a third thing
again and is not what either of the others means. Nobody should re-introduce the
second or third on the grounds that the first needs them; it does not. Worth
noting for later, though, that relay fan-out is one credible answer to broadcast
cost at very large scale, and that it meets the same per-recipient cryptography
question recorded above rather than escaping it.

## The long game, and why the layers compose

Glenn, 2026-08-21, verbatim: "yes, the whole idea eventually, is that I think a
relay network can do an effecient multicast from a game server" — "so the game
server itself can just offer a multicast flow".

That is what the library's original name meant, and it is the destination the v1
deferral leaves room for. A server publishes one stream; a relay network fans it
out; the server's upstream stops scaling with player count.

The part worth holding onto is that this only works because of a decision made
in a different layer. The delta design encodes ONE payload and broadcasts it, so
every client receives identical bytes. Had deltas stayed per-client, which is
the better-bandwidth design and the one the Titanfall and Apex lineage uses,
relay multicast would be impossible on principle, because there would be no
shared payload to fan out. So the layers compose into one story for very large
player counts: encode once, which wins the server's CPU and cache; send once to
a relay, which wins the server's upstream bandwidth; let the relay fan out,
which wins the server's IO. Each layer removes a different ceiling, and the
lowest remaining ceiling is what the next one attacks.

It also makes the cryptography question from earlier load-bearing rather than
academic. A relay can only fan out bytes that are identical for every recipient,
so per-client sealing of the bulk payload is not merely expensive under this
plan, it is disqualifying. The shape that survives is the standard one: seal the
bulk once under an ephemeral key, fan that out through the relay, and deliver
the ephemeral key to each client wrapped under their own key over their own
unicast connection, so the relay carries ciphertext it cannot read and cannot
forge. Small per client, one copy on the wire.

This is a sketch of a direction, not a design. It stays a sketch until someone
who does cryptography for a living has looked at it, and until the relay's trust
and latency properties are worked out. But it is the reason the v1 removals are
deferrals: v1 should be built so this remains possible, and specifically so that
nothing in the API assumes a payload is sealed for exactly one recipient.

Completing the picture, verbatim: "the idea of clients subscribing to particular
multicast flows" — "and then the server just handles the sending of it."

So the model is publish and subscribe. The server publishes flows and sends each
one once, without enumerating recipients; clients subscribe to the flows they
care about; the relay network delivers each flow to its subscribers.

This resolves a trade Glenn named as forced only a day earlier. Per-client deltas
give the best bandwidth and the worst server CPU, since the server builds a
different payload for every client. Global broadcast gives the best CPU and the
worst bandwidth, since every client receives everything whether it matters to
them or not, and taking that trade is what the thousand-player target required.
Flows are the third position: the server encodes once per flow, so CPU scales
with the number of flows rather than the number of clients, and each client
receives only what it subscribed to, so bandwidth scales with interest rather
than with the size of the world. His own stated wish, that something might both
reduce bandwidth and not spend too much CPU, is what this shape delivers.

Two consequences to carry into any design work.

Interest management stays game logic. Deciding which flows a client should be
subscribed to is a question about the game's world, and the transport should
carry subscriptions rather than compute them, or the no-structure rule is broken
in a new place.

Subscription changes are a key management problem, and this is the unglamorous
part that decides whether the scheme is sound. If each flow's bulk is sealed
under a flow key, then a client that unsubscribes still holds that key, and a
client that subscribes late could read what it recorded earlier. Whether that
matters depends on the game and the flow, and the honest answers range from not
caring, through rekeying a flow when its membership shrinks, to accepting a
window. It is a well-studied problem with well-known costs. It is not one to
improvise, and its cost belongs in the design rather than in a later surprise.

## Why this is the big one

Glenn, 2026-08-21, verbatim: "eventually, this multicast moves out to a relay
network" — "(network next like)." — "it's big I think." — "this idea is big." —
"it's the natural next evolution of network next."

Stated plainly so it can be argued with rather than admired: Network Next
optimized the PATH that a fixed quantity of traffic took, and won milliseconds.
This optimizes the QUANTITY, and wins a factor of the player count on server
egress. Those are different orders of thing. A routing improvement makes a game
feel better; removing an O(players) term from a server's outbound bandwidth
changes what a game costs to run and therefore what a game can be.

The reason it is available now and was not before is worth stating, because it
explains the word "evolution". A relay can only fan out a payload that is
identical for every recipient. Network Next had the network and no such payload,
because per-client deltas are different bytes for every client by construction.
The broadcast delta model built for the space game supplies exactly the missing
half: one encoded payload, identical for everyone, already proven in production
under the constraint that forced it. Whoever holds both halves can do this;
holding either alone is not enough.

What would have to be true, which is where the work is rather than the idea:

The economics have to hold, meaning relay bandwidth bought in bulk at peering
points has to be materially cheaper than game-server egress. Plausible, and a
business question to verify with real numbers rather than assume.

The cryptography has to work under fan-out, which is the group key management
problem recorded above and the single most likely place for this to be harder
than it looks.

The latency budget has to survive an extra hop, which argues that flows carry
downstream state and never upstream input, since input is where latency is felt
and where fan-out buys nothing anyway.

And it asks a game to adopt the whole stack, encoder and transport and network
together. That is a real barrier to adoption and simultaneously the reason the
combination is defensible, and it should be reasoned about as both.

## Ownership, and the separation from Network Next

Glenn, 2026-08-21, verbatim: "but we're creating it separate to network next (it
doesn't violate any IP or patents of network next) and investors in network next
didn't pay for or invest in this idea. it's separate." — "so network next stays
as it is today, improved. a network accelerator. succeed or fail on that." —
"and rocketnet is the new stuff, owned by mas bandwidth llc."

Recorded here rather than buried, because it governs the rest of this file.
Rocket Net is owned by Mas Bandwidth LLC and is separate work. Network Next
continues as it is today, improved, as a network accelerator, and succeeds or
fails on that.

The phrase "the natural next evolution of network next" appears above and
describes where the IDEA sits in a problem space. It is not a claim of
derivation, and this section governs if the two ever read as though in tension.

The practical rule for anyone working here: this repository starts from flow and
from work owned by Mas Bandwidth, and nothing from Network Next's codebase,
designs or documents comes across into it. If a question ever arises about the
provenance of a particular piece, it goes to Glenn before it goes into a commit.

Confirmed the same day: "and clients can subscribe to it." So the server offers a
flow and clients subscribe to that flow, which raises the question of who decides
whether a subscription is allowed.

It has to be the server, and the reason is not administrative. In a competitive
game, interest management IS an anti-cheat boundary: a client that may subscribe
to any flow it likes can subscribe to the flow covering the far side of the map
and read positions it should not have, which is a wallhack implemented at the
protocol level rather than in a hacked client. Free subscription is therefore not
an option regardless of how convenient it is.

The elegant part is that the cryptography already recorded above enforces this
without the relay having to. If a flow's bulk is sealed under a flow key, and the
server delivers that key only to the clients it authorizes, wrapped under their
own keys over their own connections, then the relay can fan out promiscuously to
anyone who asks and only authorized clients can read a byte of it. The relay
never holds a client key, never makes an access decision, and never has to be
trusted with one. Authorization and encryption become the same mechanism, which
is the cheapest possible way to get both.

The design consequence to hold: subscription is two separate things that must not
be conflated. Joining a fan-out is a transport and routing matter and can be
open. Receiving the key is an authorization matter and belongs to the server, and
it is the one that actually decides what a client can see.

Sharpened, verbatim: "Yes, the server subscribes/unsubscribes clients from
flows" — "for sure." — "that's definitely true." — "insecure otherwise."

So it is stronger than authorization. Subscription is an OPERATION THE SERVER
PERFORMS on a client's behalf, not a request a client makes and the server
approves. The distinction matters because it deletes an attack surface rather
than defending one: there is no client-initiated subscribe path to forge, to
replay, to rate-limit, or to get wrong in the relay. The API should not have a
client-side subscribe call at all. A client discovers what it is subscribed to
by receiving it.

The half that still wants deciding is unsubscribe. When the server removes a
client from a flow, that client still holds the flow key it was given, so unless
the flow rekeys it can keep reading anything it can still capture. Whether that
matters is per flow and per game, and the honest options are the usual three:
accept it where the content is not sensitive, rekey on membership shrink and pay
for it, or accept a bounded window. It is the group key management problem
recorded earlier, arriving at the exact place it always arrives, and it should
be priced during design rather than discovered during a playtest.


## What flow actually contains — surveyed 2026-08-21

A full structural survey was run before any implementation. Its detail lives
outside this repository because it necessarily discusses another project's
deployment; what belongs here is the conclusion, because it changes the plan.

**The starting point is not flow alone.** flow carries the platform layer, the
AF_XDP fast path, and the claim/write/commit send API — which is the single most
valuable thing in it, is genuinely zero-copy on the server, and is proven in
production. What flow does NOT carry is reliability (no sequence numbers, no
acks, no retransmit anywhere), fragmentation, most-recent-only, broadcast, or
per-packet encryption. Those exist, written and in production, one layer up in
the game that uses flow — including per-fragment acknowledgement with per-chunk
resend, which the roadmap notes below describe as a future idea and which is in
fact already running.

So RocketNet v1 is flow's IO floor and API shape PLUS that reliability and
security layer, with its serialization parameters replaced by byte pointers.
That is what "a merge of flow and bits of netcode, reliable, yojimbo" means in
practice: those bits were already merged once, and re-deriving them from
upstream would be doing it a second time.

**The design problem to settle before any code.** flow's committed frames are
reclaimed on the next tick, with the frame index recovered by pointer arithmetic
against a per-tick arena. Reliable retransmission, most-recent-only supersede,
and broadcast refcounting each require a buffer to outlive commit; broadcast
requires it to outlive commit by an unbounded amount, gated on the slowest
recipient. Every other API decision is downstream of this one. The likely shape
is an explicit opaque handle from start_packet rather than a derived index, a
refcount with a per-connection acknowledgement bitmap, and a hard occupancy
bound so one bad connection cannot pin the pool.

**Two other things the survey settled.** There is no multicast code and no relay
code anywhere in flow — not a file, not a function, not a constant — so the v1
deferral removes nothing that exists. And the public headers are not C: the two
that matter carry C++ linkage, default arguments and an overload set, so the C
ABI is mechanical work that must happen early because the batch entry point
design depends on it.

## C# is the customer's language, and that is a design input rather than a binding

Glenn, 2026-08-21, verbatim: "it will be callable from C#" — "whether it is
implemented in C# or a mix of C++ and C# (likely) is an implementation decision
we will work out, depending on performance. But it will definitely be designed
for Dean to call it from C# efficiently, because that's where Dean works right
now."

Two things to separate. **Efficiently callable from C# is a requirement**, fixed
now, and it shapes the API rather than being satisfied by a binding layer bolted
on afterwards: batch-shaped entry points instead of one call per small thing,
buffers that arrive as spans over native memory rather than marshalled copies,
and no design that assumes a transition is free. **What the library is written
in is open** — C++ throughout with a thin boundary, or a genuine mix with parts
in C# — and is decided by measurement when there is something to measure.

Worth stating why the mix is plausible rather than a compromise. The kernel-bypass
path must be native and is not negotiable. Above it, modern C# with spans,
stack allocation, unchecked pointer work and vector intrinsics is fast enough
that the question is genuinely open, and every piece implemented in C# is a
boundary crossing that does not happen for the customer whose code is C#. That
is an argument to be tested, not assumed, and the API should be designed so that
the answer can change without the interface changing.

## The API idiom

Glenn, 2026-08-21: "I desire that rocketnet is a C-like API like netcode".

So this is not only "C-callable so C# can reach it". It is netcode.io's idiom
throughout: a prefix on every symbol, opaque structs the caller only ever holds
a pointer to, plain functions taking that pointer first, creation and destruction
as explicit paired calls, an update call driven by the caller's loop, no classes,
no templates, no inheritance, no exceptions, no allocation the caller did not ask
for. A header a C compiler accepts and a binding generator can read without help.

Two reasons this is the right call beyond taste. It is the shape that has already
worked at scale in this author's own libraries, so it is proven rather than
chosen. And it is what makes the C# story cheap: a C-idiom surface binds without
a wrapper layer, and the claim-and-commit calls stay eligible for the fast
managed-to-native transition, which a surface built on classes or defaulted
arguments would not be.

The buffer ownership design already drafted (in the private engagement repo)
proposes exactly this: prefixed free functions, an opaque handle returned from
claim, and explicit release, with batch forms so a caller that has many buffers
to reclaim does not pay a transition per buffer.

And the rule for how material arrives, verbatim: "so it is easily used from other
languages like C# callable." — "So as we port across parts of flow, it should
turn into C-like API".

So porting is conversion rather than copying. flow's public headers today carry
C++ linkage, default arguments and an overload set, and none of that survives the
crossing. Each piece arrives in rocketnet already wearing the target idiom, at
the moment it arrives, rather than being moved first and cleaned up in a later
pass.

That ordering is worth defending when it feels slower, because the alternative
is a single large API rewrite at the end, performed on a body of code that by
then has consumers. Converting as you port also means every piece is callable
from C# the day it lands, so the binding is exercised continuously instead of
being discovered to be wrong late. A file that arrives still speaking C++ is
technical debt with a customer attached.

And the expectation to set before anyone is surprised by it: "this is enough of
a transform that we will surface and find bugs."

That is not a risk to mitigate, it is the mechanism working. Moving code across
a boundary forces every implicit thing to become explicit — what a function
assumed about its caller, what a member was quietly carrying, which invariant
was held by convention rather than by construction — and implicit assumptions
are exactly where bugs live. An API rewrite is a very effective bug detector
disguised as clerical work.

There is same-day evidence rather than only a principle. Retiring the space
game's forked serializer was precisely this kind of transform, and it surfaced a
live defect that had been in production networking: the fork's stream carried a
packet index as a private member, and moving that out of the stream revealed
that it stashed the compacted received-packet index while acknowledgement looked
it up by the raw receive-buffer slot, so one skipped packet made every later
acknowledgement in the batch read the wrong endpoint header. Nobody was looking
for it. The move found it. It also found signed-overflow undefined behaviour the
optimizer exploits, and a floating-point contraction hazard the build was only
avoiding by accident.

So budget for finding things, treat each one as the port paying for itself, and
do not let a discovered bug be filed as a delay.

Naming, specified: `rocketnet_client_create` and so on. The full library name as
the prefix on every public symbol, then the subject, then the verb, exactly as
netcode.io does it — `rocketnet_client_create`, `rocketnet_client_destroy`,
`rocketnet_client_update`, `rocketnet_server_create`, and the send and receive
calls in the same shape. Types follow: `rocketnet_client_t`, `rocketnet_server_t`.

Not an abbreviation. A reader of someone else's code should be able to tell what
library a call belongs to without looking anything up, which is most of what a
long prefix buys and all of what a short one throws away.

The pattern named twice over: "like netcode API, and network next API" — "this
is the pattern for how we want to deliver rocketnet" — "and the byte buffer only
design decision, makes the interface nice and portable".

Both precedents are the same shape and both are this author's, so the idiom is
proven rather than borrowed. What the last clause adds is the reason the two
decisions belong together rather than being separate preferences.

**Byte buffers are the one datatype every language already has.** Because the API
carries opaque bytes and never a message type, a schema, or a serializer, the
entire public surface reduces to pointers, lengths and integers. There is nothing
in it that a binding has to mirror, translate or keep in sync — no type hierarchy
to reproduce in C#, no serialization contract to reimplement per language, no
version skew between a library's idea of a message and a binding's. A binding
becomes a thin declaration of functions rather than a project.

So the C idiom and the byte-buffer rule reinforce each other: the idiom makes the
calls reachable from anywhere, and the byte buffer makes there be nothing else to
reach. Either one alone would be half a portable interface. Together they are why
a language port of this library should be a small piece of work rather than a
rewrite, and that property is worth defending against every future feature that
would be more convenient if the transport just understood a little bit about the
payload.

The contrast, stated precisely: "vs. yojimbo which has the need for the full
serialize streams in the interface due to messages."

That is the diagnosis, and it is worth stating without disparaging the design it
describes. yojimbo's message system was its feature, not its mistake — but a
message system means messages serialize themselves, which means the serialization
streams appear in the public interface, which means anyone binding the library to
another language must reproduce the whole serialization model before they can
send a single packet. The portability cost was not a flaw in the implementation;
it followed inevitably from where the message boundary was drawn.

RocketNet draws that boundary one layer lower and pays nothing for it, because
the layers above — serialize, delta, replicant — already exist to do the job
yojimbo's message system was doing.

Implementation language, decided the same day: "this way rocketnet is probably a
C-like C++ implementation". So C++ as the compiler, written in a C-like style,
exposing a C interface. Not idiomatic C++ with classes and templates at the
boundary, and not C for its own sake. The internals may use what C++ gives them
where it genuinely helps; the surface stays something a C compiler accepts and a
binding generator can read. Note this sits comfortably with the open question
recorded earlier about parts eventually being implemented in C# — that question
is about where a boundary crossing costs less, and the C surface is what keeps
the answer changeable.

**Bindings, not ports — and this corrects an earlier line in this file.** Glenn:
"it is just referred to from other languages through that interface, vs. being
fully re-implemented in those languages." — "which is better, because C++ is
faster and more low-level."

Earlier this record said "multi-language, with C++ and C# native first", which
reads as though RocketNet follows the serialize family's pattern of a genuine
reimplementation per language. It does not, and the distinction is the whole
point of the C interface. There is ONE implementation. Other languages reach it
through the interface rather than repeating it.

The reason is the library's entire purpose. Maximum IO means being close to the
kernel interfaces, and a reimplementation in a managed language gives that up
in exactly the place it matters most. serialize can be ported because bit packing
is arithmetic on a buffer and any language does it about as well; a transport
cannot, because its speed comes from what the platform lets you do rather than
from what you compute.

So "multi-language" here means callable from many languages, not written in many.
That is a smaller promise to keep and a much better property to have: one body
of code to fix a bug in, one place a performance win lands, and no possibility
of two implementations disagreeing on the wire.

## The Network Next SDK, reviewed as the pattern to follow

Glenn: "please review the network next SDK here: https://github.com/networknext/next" —
"find the sdk dir and review it. that's the style of API we aim for with rocketnet."
And the scope of it: "only rocketnet gets the full C-API treatment."

Read at sdk/include/next.h — **513 lines, 58 exported functions**, which is the
scale worth aiming at. A public header a person can read in one sitting.

**What to take, essentially unchanged:**

The export machinery. Every public function carries a macro resolving to
`extern "C"` plus the platform's dllexport, dllimport or visibility attribute, so
one header serves static linking, a shared library, and its consumers.

Opaque handles. `struct next_client_t;` and `struct next_server_t;` are forward
declarations only — the caller never sees a field. The only transparent structs
are configuration and statistics, which are plain data passed by pointer and
filled by the library.

The naming Glenn already specified: prefix, subject, verb. `next_client_create`,
`next_client_destroy`, `next_client_update`, `next_client_send_packet`, and the
server mirror of each.

Explicit lifecycle at two levels: a global `next_init` and `next_term`, then
`_create` and `_destroy` per object. No implicit initialization, no atexit magic.

A caller-driven `_update`. The library does not own the caller's loop.

Global hooks set as function pointers — allocator, logger, assert handler — so
the library imposes neither malloc nor a logging system on its host.

A `void * context` threaded through creation and handed back in every callback,
which is how a C API avoids globals and lets one process host several instances.

A config struct with a `next_default_config()` filler, so adding a field later
does not break existing callers.

Statistics read into a caller-provided struct returning a bool, never allocated.

**Where RocketNet must deliberately differ, and it is one place.** Next's send is
`next_server_send_packet( server, to_address, data, bytes )` — a copy-in call.
That is the right shape for a routing SDK and the wrong one here, because
claim, write, commit is the whole performance story. So RocketNet keeps every
convention above and replaces exactly that one call with the claim and commit
pair, plus the broadcast form Next has no equivalent of.

**And one thing it settles for us.** The buffer-ownership design left the
receive-side lifetime contract open, noting the fastest and the safest answers
differ. Next answers it with a callback: the received pointer is valid for the
duration of the call and not after. That is simple, enforceable, costs nothing,
and survives a managed binding cleanly because the C# side copies out or acts
within the callback and never holds a pointer the collector cannot see. It is a
strong candidate for the default, with a batch receive returning handles as the
opt-in for callers who genuinely need to hold buffers longer.

## Structure-free, by contrast with the layer above

Glenn, 2026-08-21: "rocketnet is a much more low-level and structure free
library."

Said while settling delta's character, which is the opposite: delta is
high-level and deliberately opinionated, describing the structure a game's
replication code is written inside. rocketnet holds no such opinions and must
not acquire any. Opaque bytes, a C interface, no view about what a payload means
or how a game is organised.

The rule underneath both: the lower a layer sits, the fewer opinions it should
hold, because everything above has to live with them. The higher it sits, the
more it must hold, because being higher is the offer to have decided already. An
opinionated transport is an obstacle; a structure-free high-level library is a
pile of parts. Each library should be the right one of those, and this one is
the transport.
