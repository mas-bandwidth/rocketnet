# Running the RocketNet client in a web browser — independent research

Research only. No code was written, built, run, or benchmarked for this document. Short illustrative snippets are included where they clarify an interface.

Reading conventions used throughout, as required:

- **SOURCED** — a claim with a link to a page opened on 2026-10-14, or a section of the wire standard in this repository read the same day. Source numbers like [S1] refer to §9.
- **ESTIMATED** — my arithmetic, shown where it is used. No measured figure is presented as measured.
- **OPINION** — my judgement or recommendation.
- **UNVERIFIED** — a claim I could not source from a page I opened. It is marked in place and never used as the sole basis for a recommendation.

Wire facts below come from the repository standard itself: transport is UDP only, the client source address is the connection identity, the connect token is a fixed 768 bytes, the handshake is cookie → challenge → response, every sealed packet pays 26 bytes of overhead (6-byte prefilter header, 4-byte clear sequence, 16-byte tag) plus a sealed ack block of at least 7 bytes on session packets, the default send budget is 1232 bytes with a 1426-byte receive ceiling, the replay window holds at least 4096 sequences with sequence zero invalid, packet types are 0–9, the default handshake retransmission rate is 10 Hz with a 5-second handshake deadline, and the message tier rides entirely inside payload packets. The public seam is the C API in the SDK header (client/server transport objects created from a connect token and a configuration struct carrying an ordered driver candidate list), with native drivers for portable UDP, XDP, GSO batching, and the Darwin pump described in the drivers document. Where this document disagrees with that standard, it says so plainly.

**Recommendation in one paragraph (OPINION).** Make WebTransport datagrams the primary browser carrier and a WebSocket gateway the universal fallback, with WebRTC data channels held as the reach fallback for networks that block QUIC/UDP but allow WebRTC. Keep RocketNet's own AEAD end to end inside whichever carrier is in use, so every gateway is a blind ciphertext relay that cannot read or forge game traffic. Put the carrier termination in a separate gateway process first, not embedded in the game server, so the native XDP/GSO fast path for native clients is untouched and the browser path can be scaled, restarted, and load-balanced independently. Prefer compiling the existing C++ client to WebAssembly behind a JavaScript transport adapter over re-implementing the wire in TypeScript, because the known-answer vectors then test the same code that ships natively. Two experiments in §8 are designed to overturn this recommendation if the numbers say otherwise.

## 1 Transports

The question in this section is narrow: which browser carrier can move RocketNet packets — unreliable, unordered, up to roughly 1200 bytes of payload, at 10–60 sends per second per player — with the least added latency, setup cost, and reach loss. Four carriers are covered in depth, then hybrids and excluded options.

### 1.1 Comparison at a glance

| Property | WebTransport datagrams | WebRTC data channel (unreliable) | WebSocket | Notes |
|---|---|---|---|---|
| Delivery | Unreliable, unordered [S1][S2] | Unreliable + unordered only when configured so [S3][S4] | Reliable, ordered (TCP) [S5] | RocketNet wants the first row's semantics |
| Datagram ceiling | Path-MTU bound; read `maxDatagramSize` at runtime [S1][S6] | Message-based; large messages fragment inside SCTP — UNVERIFIED ceiling per browser | No datagram ceiling; byte stream framed as messages [S5] | See per-carrier text; all ceilings must be measured (E5) |
| Congestion control | QUIC congestion control imposed by the stack [S6][S7] | SCTP congestion control over DTLS/UDP [S4] | TCP congestion control [S5] | None of these is the game's to tune from JavaScript |
| Setup to first byte | QUIC handshake + WebTransport session; 1–2 round trips ESTIMATED | Signalling + ICE + DTLS; several round trips ESTIMATED | TCP + TLS + upgrade; 2–3 round trips ESTIMATED | §1.2–§1.4; measure in E3 |
| NAT/firewall reach | Needs outbound UDP; HTTP/3 blocked in some networks [S8] | Best UDP reach via ICE/STUN/TURN, incl. TCP relay [S9][S10][S11] | Best reach: TCP 443 almost everywhere | Enterprise detail in §6 |
| Browser reach today | 91.64% global (caniuse, Sep 2026 share) [S12] | 97.26% global [S13] | 97.49% global [S14] | Versions in §6 |
| Migration | QUIC connection migration exists in the protocol [S7] | ICE restart / new connection; no seamless migration SOURCED only as absence — UNVERIFIED detail | None; reconnect | Browser exposure varies; §3 |

### 1.2 WebTransport over HTTP/3 (QUIC datagrams plus streams)

**What it is (SOURCED).** WebTransport is a W3C API that gives a page datagrams and streams to a server over HTTP/3 [S1][S15]. As read on 2026-10-14 the published W3C text is a Candidate Recommendation Snapshot (30 July 2026) with an Editor's Draft alongside it — implementable and shipped, but not yet a finished Recommendation [S15][S1]. QUIC itself provides multiplexed secure transport with connection migration [S7], HTTP/3 maps HTTP semantics onto QUIC [S17], and the QUIC DATAGRAM extension adds unreliable datagrams inside the QUIC connection [S18]. The browser API exposes a datagram duplex stream and bidirectional/unidirectional streams on one session [S1][S6].

**Support today (SOURCED, read 2026-10-14).** caniuse reports WebTransport at 91.64% global usage, supported from Chrome 97, Edge 98, Safari 26.4 (including Safari on iOS 26.4), Firefox 114, Opera 83, and Samsung Internet 18 [S12]. That Safari figure is the change that matters: WebKit's standards position on WebTransport has long been public [S19], and caniuse now records shipped support rather than intent, so as of the reading date the "Safari does not have WebTransport" objection is out of date for current OS versions — while remaining true for every older iOS/macOS release still in players' hands. Firefox support from version 114 is recorded by the same table [S12]; I did not separately open a Mozilla release note, so any claim about Firefox limitations beyond the version number is UNVERIFIED. Edge follows Chromium [S12].

**Datagrams: size, reliability, ordering (SOURCED + ESTIMATED).** Datagrams are unreliable and may arrive in any order, with size limited by the path MTU [S6]. The API surfaces the current limit as `maxDatagramSize`; a sender must read it rather than assume it [S1][S6]. ESTIMATED: on a 1500-byte Ethernet path, IPv6 + UDP + QUIC + DATAGRAM framing leaves roughly 1200–1400 bytes for the application datagram, which brackets RocketNet's 1232-byte default budget uncomfortably — the full budget will not always fit, so §3 caps the browser send budget below the native default. The exact per-browser, per-path ceiling is UNVERIFIED until E5 measures it; no specification I opened promises 1232 bytes of usable datagram payload.

**Streams (SOURCED).** The same session offers reliable streams [S1][S6]. OPINION: streams are the wrong tool for game state (they reintroduce ordering and retransmission inside one stream), but the right tool inside the carrier for the connect-token exchange and any control exchange that must arrive exactly once — see §3.

**Congestion control and the 10–60 Hz pattern (SOURCED + OPINION).** Datagrams ride QUIC congestion control: they are "encrypted and congestion-controlled" UDP-like messages [S6], and QUIC datagrams share the connection's congestion controller rather than bypassing it [S18]. The game cannot choose the controller, its parameters, or a send rate from JavaScript; it can only offer datagrams and accept that the stack may drop or delay them under congestion. OPINION: at 10–60 small packets per second this is normally far below any congestion limit and behaves like UDP; the risk is burst behaviour — a 60 Hz stream of near-MTU datagrams plus a reliable stream competing in the same connection — which is a measurement (E2), not a specification question.

**Latency and setup (ESTIMATED).** QUIC's handshake and HTTP/3 session establishment are designed for low-latency setup [S7][S17]; ESTIMATED at 1 round trip for the QUIC handshake on a fresh connection (more with address validation or a cold TLS cache), plus the WebTransport session request on that connection. There is no ICE phase and no signalling round trip. Per-packet added latency over raw UDP is the QUIC packet protection and the browser's datagram queueing — UNVERIFIED in magnitude; E2 measures it against a native UDP baseline on the same machines.

**Certificates, including `serverCertificateHashes` (SOURCED).** A WebTransport server normally needs a certificate the browser trusts for its hostname, exactly like HTTPS [S1][S6]. The constructor also accepts `serverCertificateHashes`, letting the page pin a server certificate by its SHA-256 hash [S20]. That escape hatch is deliberately narrow: the specification constrains which certificates can be accepted this way (short validity is the operative limit; secondary summaries describe an ECDSA P-256, roughly two-week-validity envelope) — I opened the constructor documentation [S20] and the specification [S1] but did not extract the full normative validity text, so the precise day count is UNVERIFIED here and must be confirmed from the specification text before anyone designs a self-signed fleet around it (see §10). OPINION: hash pinning is a development and LAN convenience, not a production certificate strategy. Production browser servers need publicly trusted certificates with automated issuance (§7), because a pinned hash couples every client update to a certificate rotation.

**NAT and firewall reach (SOURCED + UNVERIFIED).** The path is outbound UDP to the server's HTTP/3 port. Where UDP or HTTP/3 is blocked — some enterprise, school, and hotel networks, and some mobile carriers under load — the session never starts [S8 for HTTP/3 support context]. What fraction of real players that is, is UNVERIFIED; §6 refuses to invent it and E10 measures it.

### 1.3 WebRTC data channels

**What it is (SOURCED).** A data channel is an SCTP association carried over DTLS over UDP, negotiated through an `RTCPeerConnection` [S3][S4][S21]. SCTP-over-DTLS encapsulation is standardised [S21], the data-channel protocol defines how channels are opened and labelled [S4], and the browser API creates a channel with `createDataChannel` [S3].

**Unreliable, unordered mode (SOURCED).** By default a data channel is reliable and ordered — TCP-like, and wrong for game state. Unreliable service requires explicit configuration at creation: `ordered: false` together with exactly one of `maxRetransmits` (how many times a message may be retransmitted; zero means never) or `maxPacketLifeTime` (how long, in milliseconds, retransmission may continue) [S3]. Setting both is an error [S3]. OPINION: for RocketNet carriage the correct profile is `ordered: false, maxRetransmits: 0` — each RocketNet packet sent exactly once, with loss left to RocketNet's own ack/replay machinery above it. Anything else double-handles loss: SCTP retransmits stale state while the game has already moved on.

**Message size (SOURCED + UNVERIFIED).** Channels are message-oriented like the other carriers [S3][S4]. Browsers accept messages far larger than one RocketNet packet, fragmenting internally — but large messages interact badly with SCTP head-of-line behaviour on shared associations. The interoperable safe message size per browser is UNVERIFIED from the pages I opened; the design consequence does not depend on it, because §3 sends one RocketNet packet per channel message and never relies on channel fragmentation.

**ICE, STUN, TURN — cost (SOURCED).** A peer connection does not connect to an address; it gathers ICE candidates, checks connectivity, and nominates a pair [S9]. STUN lets an endpoint learn its NAT-visible address [S10]; TURN provides a relayed candidate when direct connectivity fails, at the cost of relay bandwidth and an extra hop for the session's whole life [S11]. A game client therefore needs a signalling channel (in practice HTTPS or a WebSocket) to exchange session descriptions and candidates before the first game byte moves. ESTIMATED setup: signalling (1 round trip, often more with trickle candidates) + ICE checks (1–2 round trips, worse with TURN allocation) + DTLS handshake (1–2 round trips) — several round trips and typically the slowest cold start of the three carriers, with high variance across NAT types. CPU cost is likewise the highest of the three: DTLS plus SCTP in userspace at both ends. Both figures are ESTIMATED; E3 measures setup percentiles and E7 measures server CPU.

**Congestion control and send pattern (SOURCED + OPINION).** SCTP brings its own congestion control and retransmission machinery under the channel configuration [S4]. With retransmission disabled the sender still shares one association across all channels on the connection, so a reliable channel (chat, asset manifest) can contend with the unreliable game channel. OPINION: dedicate the association to the game channel, or accept cross-channel interference and measure it (E2 includes this case).

**Reach (SOURCED).** This is WebRTC's real strength: ICE was designed to cross NATs, and TURN-over-TCP/TLS candidates can cross networks that block UDP entirely [S9][S11]. Browser support is the broadest of the datagram-capable options at 97.26% global, from Chrome 23, Firefox 22, and Safari 11 [S13]. OPINION: that combination — universal support plus the best hostile-network story — is why WebRTC stays in the design as the reach fallback even though it is the heaviest carrier to run.

### 1.4 WebSockets as fallback

**What it is (SOURCED).** WebSocket is a message-framed protocol over a single TCP connection, opened by an HTTP upgrade handshake [S5]. Support is effectively universal at 97.49% global, from Chrome 16, Firefox 11, and Safari 7 [S14].

**Head-of-line blocking (SOURCED property, OPINION consequence).** TCP delivers bytes in order or not at all [S5's framing rides that stream]. One lost segment stalls every later byte — including fresh game state that makes the lost bytes worthless — until retransmission arrives. No browser API exposes TCP controls that change this: there is no way from JavaScript to disable Nagle's algorithm, tune retransmission, or peek past a gap. OPINION: the standard mitigations are therefore all at the RocketNet layer, and they are partial: (a) send only the newest state (drop stale snapshots before enqueueing, so the queue behind a stall is short); (b) keep messages small and infrequent enough that a stall drains fast; (c) run control traffic on a second WebSocket so a state stall does not block joins and chat; (d) detect stalls from ack timing and degrade send rate rather than building a backlog. None of these removes the stall; E4 quantifies the added-delay percentiles so the fallback's quality is a number, not a hope.

**When WebSocket is the right answer anyway (OPINION).** It is the only carrier that works in locked-down networks, old browsers, most webviews, and every engine's web export (§4, §6). A game that cannot start is worse than a game with 100 ms stall spikes, so WebSocket is the guaranteed floor of the fallback ladder, not an afterthought.

### 1.5 Everything else considered

- **WebRTC through a relay by design (SFU-style).** Forcing all traffic through a TURN relay [S11] simplifies NAT handling and gives one observable hop, at permanent bandwidth cost and added latency. OPINION: useful as the WebRTC fallback's fallback; wrong as a default. UNVERIFIED: no cost figures were sourced; E7/E10 would produce them.
- **Raw QUIC from the page.** There is no browser API for a bare QUIC connection outside WebTransport [S1]. Any "QUIC-native" browser path is WebTransport, or WebAssembly code that still needs one of these carriers underneath. OPINION: do not list raw QUIC as a separate option.
- **WebSocket-plus-datagram hybrids.** Race WebTransport and WebSocket at connect time and keep the winner (§7). This is a connection strategy, not a fourth transport, and it is recommended.
- **Media-over-QUIC (MoQ).** MoQ is an IETF draft family for media distribution over QUIC/WebTransport [S22]. It is oriented to publish/subscribe media, not bidirectional game state. OPINION: watch it for spectator/replay distribution (§7); do not build game carriage on a draft.
- **Browser plugins and extensions.** Excluded by the brief, and correctly: OPINION — install friction and store review make them a non-starter for games.
- **HTTP/2 WebTransport and reliable-only modes.** The specification family includes transports beyond HTTP/3 [S1][S15]. Browser exposure is uneven and I did not source a support table for it, so any claim is UNVERIFIED; the design assumes HTTP/3 or nothing.

### 1.6 Verdict for this section (OPINION)

WebTransport datagrams are the only carrier whose semantics are RocketNet's semantics — unreliable, unordered, encrypted, congestion-controlled datagrams — without a signalling detour. WebRTC matches the semantics only after careful configuration and pays ICE/DTLS/SCTP on every connection forever. WebSocket cannot match the semantics at all but reaches everyone. That ordering — WebTransport primary, WebSocket floor, WebRTC for hostile networks — is what §2–§8 develop, and E1/E3/E10 are built to refute it if the reach or setup numbers disagree.

## 2 Server side

The server stays native. For each carrier there are only two shapes: the game server process terminates the carrier itself (embed), or a separate gateway process terminates the carrier and forwards RocketNet packets to the game server over ordinary UDP (gateway). This section compares the libraries, then the shapes.

### 2.1 Library comparison

Licences below were read from each repository's licence file on 2026-10-14. Last-activity dates and scale figures are marked UNVERIFIED where I did not open a page that states them; absence of a sourced figure is itself reported, because "known scale" claims circulate widely without sources.

| Carrier | Option | Language | Licence (SOURCED from repo) | Fit with a C++ server + vendored libsodium |
|---|---|---|---|---|
| WebRTC | libdatachannel | C/C++ | Mozilla Public License 2.0 [S23] | Direct embed candidate; C++ API, brings its own DTLS/SCTP stack |
| WebRTC | Pion (behind a shim) | Go | MIT [S24] | Cannot embed in C++; runs as a sidecar/gateway process |
| WebRTC | webrtc-rs | Rust | MIT (dual-licence family; MIT text read) [S25] | Gateway/sidecar, or FFI embed with a Rust toolchain in the build |
| WebRTC | Full platform WebRTC stack | C++ | UNVERIFIED — licence file not opened for this document | Largest build burden by reputation — UNVERIFIED; not recommended without E7 evidence |
| QUIC/WebTransport | msquic | C | MIT [S26] | Direct embed candidate; C API, platform TLS backends |
| QUIC/WebTransport | quiche | Rust (C API exposed) | BSD 2-clause style Cloudflare licence text [S27] | Embed via its C API or sidecar; needs an HTTP/3 + WebTransport layer above raw QUIC |
| QUIC/WebTransport | ngtcp2 | C | MIT [S28] | Embed candidate; QUIC + HTTP/3 libraries, TLS backend required |
| QUIC/WebTransport | lsquic | C | MIT [S29] | Embed candidate; HTTP/3 included |
| QUIC/WebTransport | s2n-quic | Rust | Apache 2.0 [S30] | Sidecar, or FFI embed with Rust in the build |
| WebSocket | uWebSockets | C++ | Apache 2.0 [S31] | Trivial embed or tiny gateway; smallest burden of any row |

Three cautions apply to the whole table. First, a QUIC library is not a WebTransport server: WebTransport needs the HTTP/3 session layer and the browser-facing API semantics [S1][S17] on top of QUIC streams/datagrams [S7][S18], and the amount of that layer each option ships varies — I did not open each project's feature documentation, so per-library WebTransport readiness is UNVERIFIED and is exactly what E1 and E6 check before any commitment. Second, every embedded option adds a second TLS stack and a second congestion-control implementation to a codebase that currently vendors libsodium and speaks one UDP protocol; that is build, audit, and upgrade burden for the life of the title. Third, MPL-2.0 (libdatachannel) is a file-level copyleft licence [S23]: compatible with shipping a linked server in most readings, but it is a different compliance conversation from the MIT/BSD/Apache rows, and the project's own licence counsel — not this document — settles it.

### 2.2 Embed versus gateway (OPINION, with ESTIMATED costs)

| Dimension | Embed in game server | Separate gateway |
|---|---|---|
| Added latency | Zero extra hops | One loopback/LAN hop; ESTIMATED well under 1 ms on one machine, dominated by two extra packet copies — measure in E6 |
| CPU per client | Carrier crypto + RocketNet crypto on game cores | Carrier crypto on gateway cores; game cores unchanged |
| Failure blast radius | Carrier crash or stall hits the game process | Gateway restart drops browser clients only; native clients unaffected |
| Scaling | Browser load scales only with game servers | Gateways scale, shard, and drain independently of game servers |
| Fast path (XDP/GSO) | At risk: carrier sockets compete with the tuned driver path | Untouched: native clients keep XDP/GSO; gateways use portable UDP toward the server |
| Certificates / load balancing | Every game server needs certs + renewal | Terminate at the gateway fleet behind ordinary TLS load balancing [S32] |
| Deploy/upgrade | Carrier library upgrades redeploy the game | Gateway upgrades independently, canaried by region |

OPINION: gateway first. The deciding argument is not latency — the added hop is small and measurable — it is isolation: the browser stack is the fastest-moving, least predictable dependency in the system (browser releases, QUIC behaviour changes, certificate automation), and embedding welds its release cadence to the game server's. Embedding becomes attractive only if E6 shows the gateway hop or the extra operational fleet is a real cost at target scale.

### 2.3 Preserving one-source-port-per-client (SOURCED constraint, OPINION design)

The standard keys a connection by client source IP and port for its whole life; a packet from another address is a packet from another client. A gateway therefore must not multiplex many browser sessions onto one UDP socket toward the game server with a shared source port. The conforming design is: one dedicated UDP socket (hence one source port) per browser session on the gateway, held for the session's life, with all carrier datagrams for that session forwarded from that socket and all replies returned on it. ESTIMATED cost: one socket plus small buffers per session — thousands of sessions per gateway are plausible, but the per-session memory and packets-per-second-per-core figures are UNVERIFIED until E6 measures them at 1,000 concurrent sessions. If a future design instead wants many sessions per source port, that is a protocol change (an explicit session identifier), and §3 lists it as requiring a standard addition — it must not be smuggled into a gateway.

Scaling across machines follows from the same rule: a session is pinned to one gateway (its source port lives there), so the load balancer must be session-affine at connect time and the game server sees ordinary, distinct UDP clients. No shared state between gateways is needed beyond token-era configuration, because the game server — not the gateway — owns connection state.

### 2.4 Load balancer and certificate story (SOURCED + OPINION)

WebTransport and WebSocket both begin as TLS connections to a hostname [S1][S5], so ordinary TLS termination, certificate automation through ACME [S32], and regional anycast/L4 balancing all apply unchanged at the gateway fleet. WebRTC has no hostname connection at all — the "load balancer" is the signalling service handing out gateway addresses plus TURN capacity [S9][S11] — which is one more moving part the WebTransport path does not need. OPINION: issue certificates only to gateways, never to game servers; native clients do not use TLS at all, and keeping certificates out of the game process removes a renewal outage class.

## 3 Protocol mapping

This section maps the token, handshake, AEAD, and replay machinery onto each carrier, and states what would have to change in the wire standard. The load-bearing decision comes first.

### 3.1 Keep RocketNet encryption end to end (OPINION, argued)

**Decision: keep RocketNet's AEAD inside the carrier on every browser path. The gateway relays ciphertext and cannot read or forge game data.**

Costs, stated honestly. Bytes: every packet pays the carrier's protection plus RocketNet's 26 bytes and at-least-7-byte ack block. ESTIMATED carrier overhead per datagram: QUIC short-header + packet protection is on the order of 20–40 bytes depending on connection-ID and packet-number lengths; DTLS record protection is on the order of 25–40 bytes; WebSocket framing is 2–14 bytes plus TCP/IP. These are ESTIMATED ranges from protocol shapes [S7][S18][S21][S5], not measurements. On a 100-byte game payload the double protection can add roughly half again the bytes on the wire. CPU: two AEAD operations per packet per direction instead of one — at game rates (a few thousand packets per second per thousand clients) ESTIMATED to be small next to the carrier stack itself, and E6/E7 measure it rather than assert it. Trust: this is what the cost buys — a compromised or merely curious gateway, load balancer, or TURN relay learns timing and sizes only; it holds no session keys, cannot forge a payload that will open, and cannot impersonate a player. The alternative — terminate RocketNet crypto at the gateway and trust DTLS/QUIC to the browser — makes every gateway a fully trusted game endpoint holding per-session keys, and turns gateway compromise into player impersonation. OPINION: for a commercial game that trade is never worth tens of bytes. A secondary benefit decides ties: one crypto story for native and browser clients means one audit, one conformance suite (§4), and no browser-specific security model to re-argue per carrier.

### 3.2 Connect token delivery and checking (SOURCED + OPINION)

The token is 768 bytes (1024 base64 characters), minted by the backend and delivered to the client over a secure channel; the client does not verify the backend signature — the server does, holding only the backend's public key, with no shared secret anywhere in the system. Browser delivery options:

- **HTTPS fetch (recommended).** The page fetches the token from the backend over HTTPS, exactly as a native client receives it. Nothing token-shaped touches a URL, a log, or a referrer header.
- **First carrier message.** The fetched token is then presented to the gateway as the first bytes on the carrier: a WebTransport stream, the first WebSocket message, or the signalling exchange for WebRTC. At 768 bytes it fits one message on every carrier.
- **URL fragment.** Technically keeps the token out of server logs (fragments are not sent), but puts a bearer credential in page-visible state and browser history handling. OPINION: refuse.
- **Cookie / custom header.** WebTransport and WebSocket offer limited header control from the page, and cookies invite CSRF-shaped confusion. OPINION: refuse as the primary channel; the explicit first-message presentation is clearer and carrier-uniform.

**How the gateway checks the token without a shared secret (the subtle point).** It cannot fully check it — and must not need to. Signature verification needs only the backend's public key, so a gateway *can* verify the Ed25519 signature and expiry cheaply and reject garbage before spending a UDP socket. But the sealed private block can be opened only by the target game server, so final admission is the server's, exactly as on the native path. OPINION design: gateway verifies signature + expiry (public-key only, no secret material on gateways), forwards the handshake, and the server performs the full open-and-derive as today. A gateway that cannot verify still works — it forwards everything and the server rejects — so verification at the gateway is a denial-of-service filter, not a trust requirement. That layering preserves the standard's "no shared secret" property end to end.

### 3.3 Handshake, cookies, and round trips (SOURCED + ESTIMATED)

Native handshake: request without cookie → cookie reply (nothing allocated) → request with cookie → challenge → response → first keep-alive. Three exchanges, retransmitted at the 10 Hz send rate under a 5-second deadline.

- **What the carrier already proved.** QUIC and DTLS both perform address validation and a cryptographic handshake before the carrier is usable [S7][S9], and WebSocket-over-TLS proves the client can receive at its address and completes TCP's handshake [S5]. The RocketNet cookie exchange's anti-spoofing and anti-amplification work is therefore largely redundant *against network attackers* on a browser path — the carrier would not have connected from a spoofed address.
- **What must stay anyway (OPINION).** Keep the cookie exchange unchanged in the first browser implementation. Three reasons: (1) the gateway is blind (§3.1), so from the game server's chair the handshake arrives over local UDP where the carrier's proof is invisible — the cookie is the server's own proof that its peer (the gateway socket) is live and authorised to consume a mapping; (2) one handshake code path for native and browser clients is worth more than one saved round trip; (3) removing it is a wire change (§3.5) that should be earned by E3 setup-time data, not assumed. ESTIMATED total cold setup on WebTransport: carrier setup (1–2 round trips) + full RocketNet handshake (3 exchanges at up to 100 ms send-rate spacing) — note the fixed 10 Hz retransmission cadence, not round-trip time, may dominate connect latency; raising the handshake send rate for browser clients is a configuration question E3 should answer.
- **WebRTC special case.** Signalling + ICE + DTLS already cost several round trips before the channel opens [S9][S10][S11]; adding three RocketNet exchanges on top is proportionally cheaper but absolutely slowest. OPINION: if any carrier earns a shortened handshake (skip-cookie fast path for gateway-forwarded sessions), WebRTC is where the saving matters — and it still waits for E3 evidence.

### 3.4 Identity, budgets, fragmentation, migration (SOURCED + ESTIMATED)

- **Identity rule.** Behind a gateway, "client address" becomes the gateway's per-session UDP source (§2.3) — stable for the session, meaningless beyond it. The server's rule survives unchanged in form; its meaning narrows to "the gateway session socket." If the gateway restarts, every browser session behind it changes address and is lost, exactly as a NAT rebinding loses a native session today. OPINION: acceptable for v1; session survival across gateway restarts is a protocol-addition conversation (§3.5), not a gateway trick.
- **1232-byte budget.** The budget must shrink on browser paths by the carrier overhead (§3.1) and respect the runtime datagram ceiling (`maxDatagramSize` [S1][S6]; SCTP message behaviour [S4]). ESTIMATED browser send budget: start at 1100 bytes and let E5 replace the estimate with measured ceilings per browser. Oversizing is not benign: an oversized QUIC datagram is dropped, not fragmented, turning a sizing error into silent state loss.
- **Fragmentation.** RocketNet fragments large payloads itself (types 0–9 include a fragment type). Carriers must therefore be used in one-packet-per-datagram/message mode and never allowed to fragment a second time underneath; double fragmentation multiplies loss (a payload survives only if every fragment does).
- **Migration.** QUIC supports connection migration across address changes [S7] — the one place a browser path could beat native RocketNet, whose sessions die on address change. Whether browsers expose useful migration behaviour to WebTransport sessions, and whether a gateway's per-session UDP socket can follow, is UNVERIFIED from the pages opened; the gateway design above pins a session to one source port, which deliberately forfeits migration in v1. OPINION: name this as the strongest future argument for a connection-ID-style protocol addition (§7), and measure real handover pain (E9) before paying for it.

### 3.5 What needs a standard addition, and what does not (OPINION, precise)

Needs **no** addition — deployable today with gateway + configuration only: WebTransport/WebSocket/WebRTC carriage of unchanged packets; reduced browser send budget (deployment constant, already how the budget is defined); gateway signature pre-check (public key only); token delivery by HTTPS fetch + first message.

Would need an addition, and should be proposed separately with measurements in hand: skipping or shortening the cookie exchange for carrier-validated sessions; a session identifier allowing many browser sessions per gateway source port or migration across gateway/address changes; any browser-specific packet type. Each touches the reserved-type discipline (types 10–255 are ignored by conforming receivers), so each can degrade gracefully — an old server ignores the new packet and the client reconnects, which is today's behaviour. Nothing in the browser programme requires breaking the native wire on day one, and OPINION: that is exactly how it should stay.

## 4 Client library

Two ways to put RocketNet logic in a page: compile the existing C++ client to WebAssembly, or re-implement the wire in TypeScript. The carrier (§1) is JavaScript either way — no browser gives WebAssembly a socket — so both designs share a JavaScript transport adapter; they differ in who owns the protocol state machine, crypto, and replay window.

### 4.1 Family A: C++ to WebAssembly behind a JavaScript adapter (recommended)

**Shape.** Emscripten compiles C and C++ to WebAssembly [S33]. The client transport object is created from a connect token and configuration through the existing C API; its driver candidate list is where the browser plugs in. Native drivers (portable UDP, XDP, GSO, Darwin pump) all carry bytes to sockets the page does not have, so the browser build registers one additional driver — a JavaScript driver — whose send path hands sealed-or-unsealed packet bytes to the chosen carrier (WebTransport datagrams, a data channel, or a WebSocket) and whose receive path injects inbound bytes for the next update. The transport above the driver — handshake, key derivation, AEAD, sequencing, replay, acks, fragmentation, and the message tier of records/channels/blocks/scheduler that rides in payloads — is the same compiled code as native. Time still enters as an argument to update, as the C API already requires, which keeps the browser build testable without real clocks.

**Size (ESTIMATED, with measurement method).** No build was made for this document, so no figure here is measured. ESTIMATED: a stripped client-only WebAssembly module (transport + message tier + libsodium) lands in the low hundreds of kilobytes compressed; the only sourced anchor in this space is the prebuilt libsodium browser distribution at roughly 290 KB standard / 375 KB sumo, minified and gzipped including both JavaScript and WebAssembly variants [S34] — a purpose-built client carrying only needed primitives should be smaller than a general-purpose distribution, but by how much is UNVERIFIED until someone builds it. Measurement method (part of E8): build with size reporting, record raw and gzip/brotli bytes for the module, the JavaScript glue, and the adapter separately, and track them in CI so growth is a reviewed event, not a surprise.

**Startup, throughput, per-packet cost (ESTIMATED + UNVERIFIED).** WebAssembly instantiation of a few hundred kilobytes is ESTIMATED at tens of milliseconds on desktop and low hundreds on a low-end phone; per-packet cost is dominated by the JavaScript↔WebAssembly boundary crossings in the adapter (one per packet each way if batched per update, one per packet naively). Batching — the adapter accumulates inbound datagrams and the driver drains them in one call per frame — is the design rule that keeps that cost flat. All magnitudes are UNVERIFIED until E8 profiles them; the architecture claim (batching bounds crossings per frame, not per packet) is OPINION grounded in the API's existing batch-shaped receive surface.

**Memory and threading.** The native design allocates fixed tables at create and nothing on hot paths; WebAssembly linear memory fits that model directly (one grow-at-create allocation). Threading is the trap: Emscripten pthreads need SharedArrayBuffer [S35], SharedArrayBuffer needs cross-origin isolation (specific `Cross-Origin-Opener-Policy` / `Cross-Origin-Embedder-Policy` response headers) [S36 context; S35], and caniuse records SharedArrayBuffer support as broad but conditional [S37]. Cross-origin isolation breaks third-party embeds, ads, and some portal requirements — a deployment cost many game portals will not pay — and at least one major engine's web export now defaults to single-threaded for exactly this reason [S38]. OPINION: ship the client single-threaded (transport update on the frame loop or in a dedicated Worker without shared memory, using message passing), and treat a threaded build as an optimisation a portal opts into, never a requirement. Web Workers themselves are universally supported [S39] and are the right home for the adapter when the game renders on the main thread; OffscreenCanvas [S40] matters only if rendering also moves off-thread, which is the game's decision, not the network library's. AudioWorklet is irrelevant to this library except as proof that real-time browser threads exist under strict no-allocation discipline — the same discipline the transport already keeps.

**Crypto choices (SOURCED).** Three options were compared against what the wire actually needs — ChaCha20-Poly1305 IETF AEAD, BLAKE2b key derivation with 8-byte contexts, X25519, SipHash-2-4, sealed boxes, and (server-side only) Ed25519. Note, following the standard precisely: the *client* never verifies the token's Ed25519 signature, so the browser client needs no Ed25519 at all; claims that it does are a common misreading. (1) *Vendored libsodium compiled with the client* — exact primitives, exact constants, known-answer vectors pass by construction; cost is module size (§4.1). (2) *The prebuilt libsodium browser distribution* [S34] — same primitives without owning a crypto build; cost is the larger general-purpose bundle and a second WebAssembly module to load. (3) *WebCrypto (`SubtleCrypto`)* [S41][S42][S43] — the platform API is universally available [S43], but its standardised algorithm registry does not cover this suite: ChaCha20-Poly1305 and the Curve25519 family have historically been absent or uneven across browsers in the WebCrypto specification line [S42], and BLAKE2b/SipHash are not WebCrypto algorithms at all. I did not open a per-browser algorithm matrix, so the exact 2026 support for X25519/Ed25519 in each browser is UNVERIFIED here — but it does not matter: without BLAKE2b key derivation and SipHash, WebCrypto alone cannot implement the wire, full stop. OPINION: option 1 for the compiled client (one module, one audit), option 2 only for the TypeScript family below. Never mix — half the keys from one library and half from another is how transcript-binding bugs are born.

**C API and driver mapping (SOURCED shape).** The seam already exists: transports are created with `rocketnet_client_transport_create(context, connect_token, config, time)`, pumped with an explicit-time update, and report their granted driver by name. A browser build keeps every signature; only the driver table changes (one `web` driver granted, named honestly so performance claims attach to it). The message-tier C API above it (channels, reliabilities, priorities) is transport-blind by design — records ride in payloads — so game code written against it needs no browser variant.

### 4.2 Family B: thin TypeScript re-implementation

**Shape.** Implement the wire — prefilter header, token handling, handshake state machine, key derivation, AEAD, sequence reconstruction, replay window (≥4096, zero invalid), ack blocks, fragmentation — as a small TypeScript library speaking to the same JavaScript carrier adapter, with crypto from the prebuilt libsodium distribution [S34] (WebCrypto alone is insufficient, §4.1).

**Honest comparison (OPINION + ESTIMATED).** Advantages: smallest download (ESTIMATED tens of kilobytes of TypeScript plus the shared crypto bundle), no Emscripten toolchain, idiomatic promises/streams for web developers, and trivial integration into web-only games. Disadvantages: a second implementation of a security-critical wire protocol, forever. Every future standard revision lands twice; every ambiguity in the text is resolved twice, possibly differently; the replay window, sequence reconstruction edge cases (the equidistant-candidate rule), and anti-amplification padding checks must match bit-for-bit. Conformance (§4.4) becomes a cross-language differential-testing programme rather than a shared-code argument. OPINION: justified only for a web-only title that will never ship natively, or as a prototyping vehicle (E1 may use it as scaffolding provided nobody mistakes the prototype for the product — the tracer-bullet rule).

### 4.3 Engines and bindings (SOURCED + OPINION)

- **C# binding / Unity / Blazor.** The repository carries a C# binding over the C ABI. Unity WebGL compiles C# to WebAssembly through its own pipeline, not Emscripten output the game controls, and browser networking there must go through JavaScript interop plug-ins — raw sockets do not exist. I did not open Unity's current networking manual page (the fetch failed), so Unity-specific API claims are UNVERIFIED; the architectural consequence is still clear OPINION: a Unity WebGL build consumes the *TypeScript/adapter* surface through a `.jslib` bridge, or a C# port of Family B — it does not consume the Emscripten module. Blazor faces the same JavaScript-interop shape. The C# binding's real browser value is API familiarity (same channel/reliability vocabulary), not binary reuse.
- **Godot.** Godot's web export requires WebAssembly + WebGL 2 [S38][S44] and its browser networking is WebSocket/WebRTC-shaped (raw UDP classes do not function in the browser) — the search-level sourcing for that statement is weaker than the export requirements, so treat protocol detail as UNVERIFIED and the export constraints as SOURCED [S38]. A Godot web game would consume the adapter through its JavaScript bridge singleton.
- **Bevy / Rust-to-WebAssembly.** No Bevy page was opened; claims are UNVERIFIED. OPINION by analogy with Family A: a Rust client compiled to WebAssembly still needs the same JavaScript carrier adapter; the seam is identical, only the language above it changes.
- **Plain canvas / web-only games.** Consume Family A as `createClient(token) → { update(now), send(...), onPayload }` with the reliability/channel vocabulary of the message tier. The page should never see a packet, a sequence number, or a key.

### 4.4 Conformance (SOURCED requirement, OPINION method)

The standard ships deterministic known-answer vectors (fixed keys, salt, nonce, ephemerals; expected derived keys, cookie MAC, and packet bytes). The browser build — either family — must run those vectors unmodified and byte-compare every output, in CI, on every browser in the §6 matrix. For Family A this is nearly free (same code); for Family B it is the admission price. Add one differential test the vectors cannot give: a browser client and a native server from the same build completing a handshake and exchanging payloads under simulated loss — described as E1/E8, run by later cards under the rule that unit tests use no real time, sockets, or network, and anything needing them is a functional test.

## 5 Measurement plan

Nothing in this section was run. It specifies the harness that turns §1–§4 opinions into numbers. A later spike implements it; unit tests in that spike use no real time, sockets, network, or outside services (time passed in, transports faked), and everything below that needs a real browser or network is explicitly a functional test.

### 5.1 Harness (OPINION, specified tightly enough to build)

- **Topology.** Three machines on one wired switch: (a) server machine running the native server plus, in gateway configurations, the gateway under test; (b) client machine running browsers under automation; (c) impairment machine (or network namespaces on (a)/(b)) applying `netem` loss/delay/jitter/reorder/rate limits. A fourth configuration repeats key tests with the client on a real phone over Wi-Fi and cellular.
- **Clocks and timestamps.** One-way latency needs synchronised clocks: use PTP on the wired LAN and report the measured clock offset alongside results; where sync is impossible (cellular), report round-trip/2 and say so. Timestamp at three points per packet — carrier send call, carrier receive event, game delivery — with a monotonic browser clock and server receive/send stamps carried in the payload.
- **Traffic.** Send rates 10, 30, 60 packets/s; payloads 100, 400, 800, 1200 bytes; durations long enough for the percentile (§5.3); background cases: idle, plus a bulk stream on the same connection (WebTransport stream / reliable channel / second WebSocket) to expose cross-stream contention.
- **Impairment matrix.** Loss 0/1/5/10%, added delay 20/50/100 ms, jitter at 10% and 30% of delay, reorder 1% and 5%, rate caps at 1/5/20 Mbit/s, burst loss (10–50 packet bursts every 10 s). Every (carrier × rate × size × impairment) cell is reported separately; no averaged "latency" headline number.

### 5.2 What is measured, per carrier

One-way latency and jitter percentiles (p50/p90/p99/p99.9); delivered-rate versus offered-rate (silent drop detection); head-of-line added delay for WebSocket (per-packet delay minus the same cell's WebTransport delay — the stall cost isolated); connection setup time (cold, warm-carrier, and resumed); packets per second per core and bytes per client on the server/gateway at 100/500/1000 concurrent clients; per-client resident memory; browser CPU (main-thread + worker + total) and, on phones, energy over a fixed 10-minute session; behaviour across NAT types (full-cone, restricted, symmetric, carrier-grade) and over cellular, including Wi-Fi→cellular handover outcome (connected / migrated / reconnected).

### 5.3 Sample sizes and pass lines (ESTIMATED method, OPINION thresholds)

Percentiles need counts: p99 needs at least 10,000 packets per cell to be more than anecdote; p99.9 needs at least 100,000. At 60 packets/s that is roughly 3 minutes and 28 minutes per cell respectively — the matrix must therefore be pruned by fixing size at 400 bytes for the long runs and using short runs for size sweeps. OPINION pass lines (the decision rules, stated before data exists): WebTransport remains primary if its p99 added latency over native UDP is under 5 ms on LAN and its delivered rate at 5% netem loss is at least 98% of offered at 60 Hz; WebSocket remains an acceptable floor if its p99.9 added delay at 1% loss stays under 250 ms with newest-only sending; WebRTC is promoted to primary fallback (ahead of WebSocket) if its hostile-network connect success exceeds WebSocket's by 10 points in E10 without failing the CPU budget in E7. Any pass line may be revised — before the runs, in writing, with reasons.

### 5.4 Browsers, versions, machines

Pin and record: current stable Chrome, Edge, Firefox, and Safari on desktop; Chrome on a mid-range Android phone; Safari on a recent iPhone and iPad (which also covers every other iOS browser's engine, §6). Record OS, browser build, machine model, and network hardware for every run; a number without its machine is not a result. Server-side runs report the granted configuration (gateway library and version, congestion-control defaults) with the same discipline the native driver ladder requires: claims attach to what actually ran.

## 6 Support matrix

Versions and shares below are SOURCED from caniuse tables read on 2026-10-14, whose usage figures derive from September 2026 global statistics [S12][S13][S14]. "Share reached" means the caniuse global-usage figure for browsers supporting the feature; it is not a measure of any one game's audience, and a game's own analytics outrank it the moment they exist. Cells I could not source are UNVERIFIED rather than filled from memory.

### 6.1 Browser by platform by transport

| Browser / platform | WebTransport since | WebRTC since | WebSocket since | Source |
|---|---|---|---|---|
| Chrome (Windows/macOS/Linux) | 97 | 23 | 16 | [S12][S13][S14] |
| Edge (Chromium) | 98 | 79 (full; partial 15–18) | 12 | [S12][S13][S14] |
| Firefox (desktop) | 114 | 22 | 11 | [S12][S13][S14] |
| Safari on macOS | 26.4 | 11 | 7 | [S12][S13][S14] |
| Chrome on Android | Current (154 in table) | Current | Current | [S12][S13][S14] |
| Firefox on Android | Current (157 in table) | Current | Current | [S12][S13][S14] |
| Safari on iOS / iPadOS | 26.4 | 11 | 6 | [S12][S13][S14] |
| Other browsers on iOS / iPadOS | WebKit-bound: same as Safari row | WebKit-bound | WebKit-bound | See §6.2 |
| Samsung Internet | 18 (WebTransport) | Supported (wide range) | 4 | [S12][S13][S14] |

Global reach by carrier on those tables: WebTransport 91.64% [S12], WebRTC peer connections 97.26% [S13], WebSocket 97.49% [S14]. HTTP/3 itself sits at 94.55% [S8] — slightly above WebTransport, which is the expected shape (the API trails the protocol). WebAssembly, needed by both client families' crypto, is at 97.24% [S45].

### 6.2 The iOS rule (SOURCED shape, OPINION consequence)

Every browser on iOS and iPadOS renders and scripts through the system WebKit engine regardless of its brand. I state this as platform knowledge rather than tying it to one opened page, so the precise policy wording is UNVERIFIED here; its observable consequence is visible in the tables themselves — Safari-on-iOS rows gate the features for the whole platform [S12][S13][S14]. Consequence: "Chrome on iPhone" does not rescue a WebKit gap. Before Safari 26.4, iOS had no WebTransport in any browser; after it, it has it in all of them (subject to each shell actually surfacing current WebKit, which embedded webviews may not — next row).

### 6.3 Webviews and embedded contexts (mostly UNVERIFIED — stated plainly)

Electron embeds Chromium, Steam's overlay/browser embeds a Chromium-derived view, Capacitor wraps the system webview, and a Unity WebGL build inside a native app runs in whatever webview hosts it. I opened no vendor page pinning the Chromium/WebKit versions or carrier availability for these shells, so every version claim here would be UNVERIFIED and I make none. The design rules that survive without versions (OPINION): feature-detect the carrier at runtime (`WebTransport` presence, data-channel creation, WebSocket) and never infer it from a user-agent string; assume a webview is older and more locked down than the standalone browser on the same device; give every embedded context the WebSocket floor. E11 audits the actual shells a title ships in.

### 6.4 What networks block (SOURCED mechanisms, UNVERIFIED rates)

The blocking taxonomy is: outbound UDP blocked entirely (kills WebTransport and direct WebRTC; TURN-over-TCP/TLS [S11] and WebSocket survive); HTTP/3/QUIC blocked or throttled while other UDP passes (kills WebTransport; WebRTC survives) [S8 context]; TLS interception with enterprise roots (WebSocket and WebTransport-over-TCP-shaped paths survive if the intercepting proxy passes them; certificate pinning via `serverCertificateHashes` [S20] breaks under interception by design and must not be the production path); WebRTC disabled by policy (kills the WebRTC fallback only). What fraction of players, schools, or enterprises sit behind each policy is UNVERIFIED — no page opened for this document measures it — and §5/E10 treat producing that number for a title's actual audience as launch-blocking work, rather than borrowing a percentage from a blog.

## 7 Ideas of your own

Each entry is marked as an idea, as required. None is a commitment; each names the experiment or decision that would earn it.

- (idea) **Happy-eyeballs transport race.** On connect, start WebTransport and WebSocket in parallel, keep whichever delivers the first authenticated handshake byte, and remember the winner per network for the session. This converts fallback selection from a timeout (seconds of staring at a spinner) into a race (one round trip). Needs a client-side rule against double-admission: present the same token once by retiring the losing attempt before the handshake completes.
- (idea) **Newest-only WebSocket mode.** A documented degraded profile for the TCP floor: the client drops unsent stale snapshots locally, caps the socket backlog at one tick of state, and surfaces "degraded" to the game's UI so players understand the session quality. Honest degradation beats pretending TCP is UDP.
- (idea) **Session-resume token.** A short-lived, server-issued resume credential (distinct from the connect token) that lets a browser client reattach after a page reload or gateway restart without a full backend round trip. This is a protocol addition and needs its own threat analysis; it is listed here so the pain that motivates it (reload = full reconnect) is named.
- (idea) **Connection-migration via reserved packet type.** The standard already reserves types 10–255 and sketches identifier-based migration at switch-time cost only. A sealed migration packet carrying a session identifier, followed by the cookie proof at the new address, would give browser (and mobile native) clients QUIC-like survival of network changes without taxing every packet. The strongest protocol idea in this document; earns its keep only if E9 shows handover pain is common in the real audience.
- (idea) **Regional blind-gateway anycast.** Gateways in each region terminating carriers and forwarding ciphertext to the nearest game server, with the backend minting tokens that name the gateway region rather than a raw server address. Keeps the one-source-port rule per gateway while making browser ingress a solved load-balancing problem.
- (idea) **Spectator and replay over WebTransport streams.** The same session that carries datagrams for players could carry reliable streams for spectators at a lower send rate, and a recorded packet log could be replayed into a browser client for debugging and spectating [S22's media-distribution direction is adjacent prior art]. Spectators need receive-mostly scale, not per-player simulation.
- (idea) **Token in the page, never in the URL.** Deliver the connect token in the HTTPS response that boots the game (embedded in the page payload or a dedicated fetch), so no credential ever appears in an address bar, a shared link, or an analytics URL. Paired with a short token lifetime, this shrinks the bearer-token exposure window that §3.2 accepts.
- (idea) **Browser-specific abuse posture.** Browser clients are trivially scriptable, so assume bots arrive through the cheapest carrier. Mitigations live at the backend (attestation of the page origin at token minting, rate limits per account, proof-of-work only if measurements demand it) — never as client-side secrecy, which a browser cannot keep.
- (idea) **Certificate automation as a launch checklist item.** ACME-issued certificates [S32] for gateway hostnames with automated renewal and alerting, tested by an expiry drill. Browser fleets die by forgotten certificates more often than by protocol bugs; make renewal boring before launch, not heroic after.
- (idea) **WebGPU-era page budget.** When the game renders with WebGPU and simulates on workers, the network client competes for the same threads. Reserve a documented CPU budget for the adapter (one worker, batched updates) and test the full page — game plus network — in E8 rather than benchmarking the client alone on an empty page.
- (idea) **Deterministic browser soak.** Replay a recorded packet-loss trace through the browser client nightly (functional tier) so carrier-behaviour changes in new browser releases surface as test failures before players find them. Browsers move monthly; the soak is the early-warning system.

## 8 Ranked R&D experiments

Ranked by (value of the answer to the choice of primary transport, fallback, and server-versus-gateway) divided by cost, cheapest decisive experiments first. Costs are ESTIMATED in worker-days for one engineer with the §5 harness available where stated. E3 and E10 are the two experiments that could overturn this document's recommendation (WebTransport primary / gateway first); each says so explicitly.

### E1 Browser page exchanges packets with a native server on a Mac (start Monday)

- Question: can a real browser page complete a RocketNet handshake with the native server through a gateway and exchange payloads, with no wire changes?
- Hypothesis: yes, via WebTransport datagrams through a thin gateway (§2.3), with the Emscripten client or a throwaway TypeScript scaffold.
- Build: a static page plus the smallest gateway that terminates WebTransport and forwards datagrams from one UDP socket per session to the native server on the same Mac; self-signed certificate with hash pinning for development only [S20]. Monday start: morning — stand up the native server and confirm a native client connects; afternoon — gateway echo (browser datagram in, UDP out, reply back) with packet captures at both hops; then swap the echo payload for the real handshake bytes.
- Measurement and machine: handshake completion, then 10 minutes of 30 Hz echo at 400 bytes, packet capture diffed hop by hop; one Mac, current Chrome.
- Cost (ESTIMATED): 3 worker-days.
- Dead-end criterion: the handshake cannot be carried without modifying server wire behaviour, or datagram ceilings force packets below 400 usable bytes on a LAN.
- Unblocks: everything; it is the tracer bullet whose failures become the invariants of every later experiment.
- Topic: §1 WebTransport, §2 gateway, §3 mapping.

### E2 Latency and jitter bake-off, all carriers

- Question: what latency/jitter does each carrier add over native UDP at 10/30/60 Hz and 100–1200 bytes?
- Hypothesis: WebTransport within a few ms of UDP on LAN; WebRTC measurably higher; WebSocket close until loss appears.
- Build: §5 harness with three-point timestamping; bulk-stream contention case included.
- Measurement and machine: wired LAN trio (§5.1), p50–p99.9 per cell, 100k packets for long cells.
- Cost (ESTIMATED): 5 worker-days including harness.
- Dead-end criterion: harness clock error exceeds the carrier differences being measured (then fix clocks before believing any row).
- Unblocks: primary-carrier confirmation; §5 pass lines scored.
- Topic: §1, §5.

### E3 Connection setup time and the cookie question (could overturn the recommendation)

- Question: what dominates cold connect time per carrier, and would shortening the RocketNet handshake for carrier-validated sessions change the ranking?
- Hypothesis: the fixed 10 Hz handshake cadence, not carrier setup, dominates WebTransport cold starts.
- Build: instrumented connects (cold/warm/resumed), a prototype skip-cookie gateway mode behind a flag for comparison only.
- Measurement and machine: 1,000 cold connects per carrier on LAN + 50 ms netem; time-to-first-payload percentiles.
- Cost (ESTIMATED): 3 worker-days after E1.
- Dead-end criterion: setup differences under 100 ms at p99 across carriers — then stop optimising setup and keep the full handshake everywhere.
- Unblocks: §3.3 decision; whether any standard addition is even proposed.
- Overturn potential: if WebRTC or WebSocket setup beats WebTransport decisively after a shortened handshake, the primary-carrier ordering must be re-argued.
- Topic: §3.

### E4 WebSocket head-of-line quantification

- Question: how bad is the TCP floor, exactly, under loss?
- Hypothesis: newest-only sending keeps p99.9 added delay under the §5.3 line at 1% loss and fails it at 5%.
- Build: WebSocket gateway mode in the §5 harness; naive versus newest-only send policies.
- Measurement and machine: added-delay percentiles versus the same cell's WebTransport row, loss 0/1/5/10%.
- Cost (ESTIMATED): 2 worker-days after E2.
- Dead-end criterion: even naive sending passes at 5% loss (then drop the newest-only complexity) — or newest-only still fails at 1% (then WebSocket is emergency-only and WebRTC's priority rises).
- Unblocks: fallback quality promise to game teams.
- Topic: §1 WebSocket, §5.

### E5 Datagram ceiling census

- Question: what is the true usable datagram/message ceiling per browser, carrier, and path?
- Hypothesis: at least one mainstream browser/carrier path sits below the 1232-byte native budget.
- Build: ceiling-probing page (binary search on send size, confirm delivery), run across the §6 matrix on LAN and consumer paths.
- Measurement and machine: maximum reliably-delivered size per cell; failure mode recorded (drop vs error vs truncate).
- Cost (ESTIMATED): 2 worker-days.
- Dead-end criterion: every cell fits 1232 bytes with headroom (then keep the native budget and delete the browser cap).
- Unblocks: the §3.4 browser send budget as a measured constant.
- Topic: §1, §3.

### E6 Gateway scale and the embed question

- Question: what do 1,000 concurrent browser sessions cost a gateway (pps/core, memory/session), and does embedding beat it enough to matter?
- Hypothesis: a gateway core sustains the target with headroom; embedding saves under 20% CPU at the price of §2.2's coupling.
- Build: load generator driving 1,000 sessions at 30 Hz through the gateway; an embedded-carrier build of the server for A/B.
- Measurement and machine: server-class Linux machine; CPU, memory, p99 forwarding delay for gateway versus embed.
- Cost (ESTIMATED): 6 worker-days.
- Dead-end criterion: gateway forwarding adds over 1 ms p99 or halves per-core throughput — then embedding becomes the default and §2.2 flips.
- Unblocks: server-versus-gateway decision with numbers.
- Topic: §2.

### E7 WebRTC server cost (libdatachannel versus sidecar)

- Question: what does the WebRTC fallback cost per client in CPU and operational parts, in embed and sidecar forms?
- Hypothesis: it is the most expensive carrier per client by a wide margin, justifying fallback-only status.
- Build: data-channel echo through an embedded C++ stack and through a Go sidecar, same harness as E6 at reduced scale (200 sessions).
- Measurement and machine: CPU/session, setup success across NAT types, TURN-relayed session cost.
- Cost (ESTIMATED): 6 worker-days.
- Dead-end criterion: WebRTC cost approaches WebTransport's (then simplify: promote WebRTC, drop a carrier) or exceeds 5× it (then WebRTC ships only if E10 demands it).
- Unblocks: fallback staffing and TURN budgeting.
- Topic: §1 WebRTC, §2.

### E8 WebAssembly client size, startup, and conformance

- Question: what do download size, startup time, and per-frame CPU actually cost for the compiled client, and does it pass the vectors everywhere?
- Hypothesis: low-hundreds-of-KB compressed, sub-100 ms desktop instantiation, vectors green on all §6 browsers.
- Build: client-only Emscripten build with the JavaScript driver; size reporting in CI; vector runner page.
- Measurement and machine: raw/gzip/brotli bytes, cold/warm startup on desktop + low-end Android, frame CPU at 60 Hz, vector results per browser.
- Cost (ESTIMATED): 5 worker-days.
- Dead-end criterion: compressed size over 1 MB or phone startup over 500 ms after honest optimisation — then Family B (TypeScript) is re-opened as the primary.
- Unblocks: §4 family decision; portal size budgets.
- Topic: §4.

### E9 Handover and migration reality check

- Question: how often do real sessions change network mid-game, and does any browser carrier survive it?
- Hypothesis: rare on desktop, routine on mobile; v1 (reconnect) is acceptable if reconnect is fast and rare.
- Build: phone test rig switching Wi-Fi/cellular mid-session per carrier; reconnect-time instrumentation.
- Measurement and machine: two phones, two carriers' networks; survival rate, reconnect p50/p99, session-loss rate.
- Cost (ESTIMATED): 3 worker-days (mostly logistics).
- Dead-end criterion: handover loss under 0.1% of sessions (then the §7 migration idea is shelved indefinitely).
- Unblocks: whether the migration protocol addition (§3.5) is proposed at all.
- Topic: §3 migration, §7.

### E10 Hostile-network reach census (could overturn the recommendation)

- Question: for the title's real audience, what fraction can use each carrier?
- Hypothesis: WebTransport reaches at least 90% of connect attempts; failures concentrate in identifiable networks.
- Build: connect-probe page attempting all carriers, deployed to the game's real web properties; privacy-reviewed aggregate telemetry (carrier, success, network class — no addresses retained beyond the probe).
- Measurement and machine: production web audience over at least two weeks including weekdays (school/enterprise hours).
- Cost (ESTIMATED): 4 worker-days plus review time.
- Dead-end criterion: probe bias unfixable (e.g. only reachable users can load the probe) — then fall back to lab panels and say reach stays UNVERIFIED.
- Unblocks: the fallback ordering with audience data instead of global shares.
- Overturn potential: if WebTransport success trails WebSocket by over 15 points in the real audience, WebSocket becomes primary and WebTransport the performance tier.
- Topic: §1 reach, §6.

### E11 Webview and engine-shell audit

- Question: in the shells the title actually ships (Electron build, Steam, Capacitor wrapper, Unity WebGL, Godot web), which carriers exist?
- Hypothesis: at least one shipped shell lacks WebTransport or pins an old engine.
- Build: feature-detection probe run inside each shell; a Unity `.jslib` and Godot bridge smoke test of the adapter surface.
- Measurement and machine: the actual store builds on target devices; carrier presence + E1 echo per shell.
- Cost (ESTIMATED): 3 worker-days.
- Dead-end criterion: a must-ship shell supports only WebSocket (then the WebSocket profile in §7 becomes a launch feature, not a fallback).
- Unblocks: §6.3 unknowns; engine integration plan.
- Topic: §4 engines, §6.

### E12 Double-encryption cost audit

- Question: what does keeping RocketNet AEAD inside carrier crypto (§3.1) cost in bytes and CPU at scale?
- Hypothesis: under 15% total CPU at target rates — cheap for gateway blindness.
- Build: carrier-only versus double-crypto A/B in the E6 rig; byte counts from captures.
- Measurement and machine: server-class machine at 1,000 sessions; CPU delta and wire-byte delta per payload size.
- Cost (ESTIMATED): 2 worker-days after E6.
- Dead-end criterion: double crypto costs over 30% throughput (then the termination debate reopens with data, and gateway trust must be re-argued explicitly).
- Unblocks: closing §3.1 permanently.
- Topic: §3.

## 9 Sources

All pages below were opened on 2026-10-14. Repository licence files are cited as the raw licence text in each project's repository, read the same day. The wire-standard facts (token size, handshake, overheads, budgets, replay, packet types, timing defaults, message tier, C API shape) are sourced from the standard, channels design, SDK header, and drivers document in this repository at the base commit of this research, read 2026-10-14; they are cited in prose rather than by number.

- [S1] W3C WebTransport — Editor's Draft — API for datagrams and streams over WebTransport — https://w3c.github.io/webtransport/
- [S2] Using WebTransport (Chrome for Developers) — datagrams as encrypted, congestion-controlled UDP-like messages; MTU-bound size — https://developer.chrome.com/articles/webtransport
- [S3] MDN: RTCPeerConnection.createDataChannel() — ordered, maxPacketLifeTime, maxRetransmits semantics and mutual exclusion — https://developer.mozilla.org/en-US/docs/Web/API/RTCPeerConnection/createDataChannel
- [S4] RFC 8831: WebRTC Data Channels — SCTP-based data transport for WebRTC — https://www.rfc-editor.org/rfc/rfc8831.html
- [S5] RFC 6455: The WebSocket Protocol — message framing over one TCP connection after an HTTP upgrade — https://www.rfc-editor.org/rfc/rfc6455.html
- [S6] MDN: WebTransport API — session, datagram, and stream surface — https://developer.mozilla.org/en-US/docs/Web/API/WebTransport_API
- [S7] RFC 9000: QUIC transport — multiplexed secure transport with connection migration and low-latency establishment — https://www.rfc-editor.org/rfc/rfc9000.html
- [S8] caniuse: HTTP/3 protocol — browser support and 94.55% global usage (Sep 2026 share) — https://caniuse.com/http3
- [S9] RFC 8445: Interactive Connectivity Establishment (ICE) — candidate gathering, connectivity checks, nomination — https://www.rfc-editor.org/rfc/rfc8445.html
- [S10] RFC 8489: Session Traversal Utilities for NAT (STUN) — learning NAT-visible addresses, connectivity checks — https://www.rfc-editor.org/rfc/rfc8489.html
- [S11] RFC 8656: Traversal Using Relays around NAT (TURN) — relayed candidates when direct connectivity fails — https://www.rfc-editor.org/rfc/rfc8656.html
- [S12] caniuse: WebTransport — Chrome 97 / Edge 98 / Safari 26.4 / Firefox 114, 91.64% global — https://caniuse.com/webtransport
- [S13] caniuse: WebRTC Peer-to-peer connections (RTCPeerConnection) — Chrome 23 / Firefox 22 / Safari 11, 97.26% global — https://caniuse.com/rtcpeerconnection
- [S14] caniuse: Web Sockets — Chrome 16 / Firefox 11 / Safari 7, 97.49% global — https://caniuse.com/websockets
- [S15] W3C WebTransport — Candidate Recommendation Snapshot, 30 July 2026 — https://www.w3.org/TR/webtransport/
- [S17] RFC 9114: HTTP/3 — HTTP semantics mapped over QUIC — https://www.rfc-editor.org/rfc/rfc9114.html
- [S18] RFC 9221: An Unreliable Datagram Extension to QUIC — unreliable datagrams inside a QUIC connection — https://www.rfc-editor.org/rfc/rfc9221.html
- [S19] WebKit standards position on WebTransport (issue tracker) — WebKit's public position, read alongside the shipped-support table [S12] — https://github.com/WebKit/standards-positions/issues/18
- [S20] MDN: WebTransport constructor — including the serverCertificateHashes option — https://developer.mozilla.org/en-US/docs/Web/API/WebTransport/WebTransport
- [S21] RFC 8261: DTLS Encapsulation of SCTP Packets — how SCTP rides over DTLS — https://www.rfc-editor.org/rfc/rfc8261.html
- [S22] IETF Datatracker: Media over QUIC Transport draft — MoQ draft family status and direction — https://datatracker.ietf.org/doc/draft-ietf-moq-transport/
- [S23] libdatachannel repository licence — Mozilla Public License 2.0 text — https://raw.githubusercontent.com/paullouisageneau/libdatachannel/master/LICENSE
- [S24] Pion WebRTC repository licence — MIT text — https://raw.githubusercontent.com/pion/webrtc/master/LICENSE
- [S25] webrtc-rs repository licence — MIT text (dual-licence family) — https://raw.githubusercontent.com/webrtc-rs/webrtc/master/LICENSE-MIT
- [S26] msquic repository licence — MIT text — https://raw.githubusercontent.com/microsoft/msquic/main/LICENSE
- [S27] quiche repository licence — BSD 2-clause style Cloudflare text — https://raw.githubusercontent.com/cloudflare/quiche/master/COPYING
- [S28] ngtcp2 repository licence — MIT text — https://raw.githubusercontent.com/ngtcp2/ngtcp2/main/COPYING
- [S29] lsquic repository licence — MIT text — https://raw.githubusercontent.com/litespeedtech/lsquic/master/LICENSE
- [S30] s2n-quic repository licence — Apache License 2.0 text — https://raw.githubusercontent.com/awslabs/s2n-quic/main/LICENSE
- [S31] uWebSockets repository licence — Apache License 2.0 text — https://raw.githubusercontent.com/uNetworking/uWebSockets/master/LICENSE
- [S32] Let's Encrypt: Getting Started — certificates via the ACME protocol and ACME clients — https://letsencrypt.org/getting-started/
- [S33] Emscripten: About Emscripten — compiling C and C++ to WebAssembly — https://emscripten.org/docs/introducing_emscripten/about_emscripten.html
- [S34] libsodium browser distribution README — roughly 290 KB standard / 375 KB sumo, minified and gzipped, JS + WebAssembly — https://raw.githubusercontent.com/jedisct1/libsodium.js/master/README.md
- [S35] Emscripten: Pthreads support — threading requirements including SharedArrayBuffer — https://emscripten.org/docs/porting/pthreads.html
- [S36] caniuse: SharedArrayBuffer — broad but conditional support; isolation-dependent availability — https://caniuse.com/sharedarraybuffer
- [S37] caniuse: SharedArrayBuffer (support table, same reading as [S36]) — version-by-version conditional support relied on in §4 — https://caniuse.com/sharedarraybuffer
- [S38] Godot Engine documentation: Exporting for the Web — WebAssembly/WebGL 2 requirements and single-threaded default rationale — https://docs.godotengine.org/en/stable/tutorials/export/exporting_for_web.html
- [S39] caniuse: Web Workers — effectively universal worker support — https://caniuse.com/webworkers
- [S40] caniuse: OffscreenCanvas — worker canvas support by browser — https://caniuse.com/offscreencanvas
- [S41] MDN: Web Crypto API — the SubtleCrypto platform surface — https://developer.mozilla.org/en-US/docs/Web/API/Web_Crypto_API
- [S42] W3C Web Cryptography specification — the standardised WebCrypto algorithm registry — https://www.w3.org/TR/WebCryptoAPI/
- [S43] caniuse: SubtleCrypto API — universal availability of the API itself (not of every algorithm) — https://caniuse.com/mdn-api_subtlecrypto
- [S44] caniuse: WebGL 2.0 — engine web-export graphics baseline, 97.13% global — https://caniuse.com/webgl2
- [S45] caniuse: WebAssembly — support and 97.24% global usage — https://caniuse.com/wasm
- [S46] RFC 9001: Using TLS to Secure QUIC — how TLS 1.3 secures QUIC connections — https://www.rfc-editor.org/rfc/rfc9001.html

## 10 Not done

What could not be read or settled in this pass, and what to do next, in the order it should be done:

- **No measurements exist yet.** Every latency, CPU, memory, size, and battery figure in §1–§4 is ESTIMATED or UNVERIFIED by design. Next: E1, then the §5 harness (E2). Do not quote any number from this document as a result.
- **Server-library readiness and activity were not verified per project.** Licences were read [S23]–[S31], but WebTransport feature completeness, last-activity dates, and scale figures for each library were not opened and are UNVERIFIED. Next: E1/E6/E7 check the two or three finalists hands-on before any adoption.
- **The exact `serverCertificateHashes` validity envelope** (certificate type and maximum validity) was not extracted from the normative specification text. Next: read the certificate-hashing section of [S1]/[S15] directly before designing any self-signed or private-CA deployment; production should use ACME certificates [S32] regardless.
- **A per-browser WebCrypto algorithm matrix** (X25519, Ed25519, ChaCha20-Poly1305 per browser and version) was not opened. It does not change the §4 conclusion — BLAKE2b and SipHash are outside WebCrypto, so the vendored library is required — but E8 should record the matrix so the TypeScript family's options are stated precisely.
- **Unity and Bevy browser details are UNVERIFIED.** The Unity manual fetch failed and no Bevy page was opened. Next: E11 audits the actual engine shells and store builds rather than manuals.
- **Webview versions (Electron, Steam, Capacitor) are UNVERIFIED.** No vendor version page was opened. Next: E11's in-shell probes replace version archaeology with detection.
- **Hostile-network rates are UNVERIFIED.** No source measures what real players' networks block. Next: E10's audience census; until then §6's taxonomy must not be read as frequencies.
- **Firefox and Safari carrier quirks beyond version numbers** (datagram behaviour, stream limits, migration exposure) were not separately sourced. Next: E5's census records behaviour per browser instead of trusting tables.
- **TURN economics** (relay bandwidth cost per concurrent player) were not sourced. Next: E7 measures relayed-session cost; procurement numbers come from the chosen relay deployment, not this document.
- **The iOS all-browsers-use-WebKit statement** is platform knowledge whose precise current policy wording was not tied to an opened page. Next: confirm against current platform documentation during E11, where its only consequence (feature detection, §6.3) is applied anyway.

