# RocketNet in the browser — independent research (aurora)

Research only; no code written, nothing built or benchmarked. Read date for all web sources: 2026-10-14. Labels: **SOURCED** = link opened this session, **ESTIMATED** = arithmetic shown, **OPINION** = judgement, **UNVERIFIED** = could not source. Wire facts are from the staged checkout at `sprint/rocketnet-browser-research` (11a39bf): STANDARD.md, docs/CHANNELS.md, sdk/rocketnet.h, DRIVERS.md, README.md.

Recommendation in one paragraph (**OPINION**): primary = WebTransport datagrams into a native gateway that relays ciphertext to the untouched UDP server (one UDP source port per browser session); fallback = WebRTC data channels (unordered, `maxRetransmits: 0`) through the same gateway shape for networks/browsers where HTTP/3 is blocked; last resort = WebSocket gateway for reach only, never for fast play. Keep RocketNet AEAD end-to-end inside every carrier. Client = existing C++ compiled to WASM behind a JS driver first; TypeScript re-implementation second, only if size/startup measurements demand it. Experiments E1–E12 in §8 are ranked to overturn this if wrong.

## 1 Transports

All browser-status claims below were read 2026-10-14 from caniuse and the W3C/MDN pages listed in §9. caniuse global shares quoted are caniuse's own figures (StatCounter September 2026 vintage shown on the pages when opened).

### Comparison at a glance

| Property | WebTransport datagrams | WebTransport streams | WebRTC data channel (unreliable) | WebSocket (wss) |
|---|---|---|---|---|
| Carrier | QUIC datagrams over HTTP/3/UDP [S1][S2] | QUIC streams over HTTP/3 [S1] | SCTP over DTLS over UDP (ICE) [S5][S6] | TCP (TLS) [S8] |
| Unreliable delivery | Yes, datagram API [S1] | No (reliable streams) | Yes: `ordered:false, maxRetransmits:0` [S7] | No |
| Ordering | None across datagrams | Per-stream ordered | Configurable; unordered mode available [S7] | Total order, one stream |
| Datagram/message ceiling | Implementation QUIC datagram size; must be probed (E2) UNVERIFIED exact value | Stream bytes, no game-sized ceiling | SCTP message; browser send caps differ; probe (E3) UNVERIFIED | No useful game ceiling |
| Congestion control imposed | QUIC CC on the connection; datagrams share it [S2][S3] | Same QUIC CC | SCTP CC + DTLS/ICE path; browser-owned | TCP CC; loss stalls all later bytes |
| Setup | HTTP/3 + QUIC handshake; 1-RTT class, plus TLS [S2][S3] | Same connection as datagrams | SDP offer/answer via signalling + ICE + DTLS: most round trips | TCP+TLS+HTTP Upgrade: few round trips |
| NAT/firewall reach | Needs UDP/HTTP/3 egress; blocked where UDP or H3 is blocked | Same | Best UDP reach via STUN/TURN; TURN-TCP/TLS fallback exists [S10][S11] | Best reach: TCP 443 almost always passes |
| Head-of-line blocking | None across datagrams | Across bytes in one stream only | None in unordered/unreliable mode | Full: one lost segment blocks everything |

### WebTransport (HTTP/3, QUIC datagrams and streams)

The W3C WebTransport API (Candidate Recommendation Snapshot 30 July 2026 when opened) exposes unreliable datagrams and reliable bidirectional/unidirectional streams over HTTP/3 [S1](https://www.w3.org/TR/webtransport/). The protocol runs over QUIC (RFC 9000) and HTTP/3 (RFC 9114) [S2](https://datatracker.ietf.org/doc/html/rfc9000) [S3](https://datatracker.ietf.org/doc/html/rfc9114). MDN documents the `WebTransport` constructor and datagram readable/writable streams [S4](https://developer.mozilla.org/en-US/docs/Web/API/WebTransport/WebTransport) and the API overview [S5](https://developer.mozilla.org/en-US/docs/Web/API/WebTransport).

Support read 2026-10-14 from caniuse [S6](https://caniuse.com/webtransport): Chrome 97+, Edge 98+, Firefox 114+, Safari 26.4+ (including Safari on iOS 26.4+), Opera 83+, Samsung Internet 18+; global 91.64%. HTTP/3 itself is broader and older (Chrome 87+, Firefox 88+, Safari 16+) at 94.55% [S7](https://caniuse.com/http3), but HTTP/3 support does not imply the WebTransport JS API — the API row above governs. **ESTIMATED:** WebTransport reach ≈91.6% of browsers by caniuse's measure today, rising as pre-26.4 Safari ages out; it is no longer a Chromium-only story, which is the single biggest change from received wisdom and the reason this document ranks it first.

Datagram size: QUIC datagrams are bounded by the peer's advertised max datagram frame size and the path MTU; the browser does not promise RocketNet's 1232-byte budget fits. Exact per-browser maxima are **UNVERIFIED** here and are experiment E2's job. Design rule (**OPINION**): the client must query/negotiate and clamp its send budget to `min(1232, carrier_max − overheads)`; never assume 1232 survives.

Congestion control vs a 10–60 Hz game pattern: QUIC applies connection-level congestion control and pacing. At 60 Hz with 100–1200-byte payloads (ESTIMATED: 60×1200×8 ≈ 576 kbit/s plus overheads) a healthy path is far below typical CC limits, but a lossy path will see QUIC throttle datagram sends and the browser may drop datagrams it cannot send promptly. That drop is desirable (stale state dies), but it is browser-paced, not game-paced: the game cannot choose which datagram dies. Mitigation (**OPINION**): keep per-packet payloads self-contained (RocketNet already does), coalesce less, and treat `datagrams` writable-stream backpressure signals as the send gate.

Certificate handling: a WebTransport server needs a publicly trusted TLS certificate for the origin, or the page may use `serverCertificateHashes` with a short-lived self-signed certificate hash pinned in JS. The hash path avoids CA cost for dedicated game servers but carries lifetime/rotation limits in the spec and uneven historical support; treat CA certificates via automated issuance as the production default and hashes as a dev/private-server convenience (**OPINION**; spec surface in [S1](https://www.w3.org/TR/webtransport/), constructor options in [S4](https://developer.mozilla.org/en-US/docs/Web/API/WebTransport/WebTransport)). Certificate automation is revisited in §7.

Connection setup: QUIC's handshake is 1-RTT (0-RTT resumption exists in QUIC but must not carry RocketNet handshake bytes that need replay care; see §3). Add DNS/TLS-equivalent costs already inside QUIC. Expect setup well under WebRTC's multi-phase setup and comparable to or better than wss on cold connections; exact milliseconds are workload/network dependent and **UNVERIFIED** until E1/E4 measure them.

### WebRTC data channels

WebRTC is a W3C Recommendation (13 March 2025 version opened) [S8](https://www.w3.org/TR/webrtc/). Data channels are SCTP over DTLS (RFC 8831) with ICE (RFC 8445), STUN and TURN (RFC 8656) for NAT traversal, and the security architecture in RFC 8827 [S9](https://datatracker.ietf.org/doc/html/rfc8831) [S10](https://datatracker.ietf.org/doc/html/rfc8445) [S11](https://datatracker.ietf.org/doc/html/rfc8656) [S12](https://datatracker.ietf.org/doc/html/rfc8827). SDP multiplexing is RFC 9143 [S13](https://datatracker.ietf.org/doc/html/rfc9143).

Reliability knobs (MDN `createDataChannel`, opened) [S14](https://developer.mozilla.org/en-US/docs/Web/API/RTCPeerConnection/createDataChannel): `ordered` (default true), `maxRetransmits`, `maxPacketLifeTime`; the latter two are mutually exclusive. For RocketNet payloads the mapping is `ordered:false, maxRetransmits:0` — fire-and-forget, RocketNet's own sequence/ack/replay machinery stays authoritative. Partial reliability with a small `maxPacketLifeTime` is the alternative when SCTP-level timely expiry is preferred over a hard retransmit count; E3 compares them.

Costs: signalling (SDP offer/answer over HTTPS/wss — an extra server round trip the game must operate), ICE candidate gathering and checks, DTLS handshake. ESTIMATED setup: several round trips before first game byte, the slowest of the three carriers, and the most code (signalling server, ICE policy, TURN credentials). CPU: DTLS+SCTP in the browser is mature and hardware-friendly, but per-message SCTP overhead and browser buffering behaviour at 60 Hz small messages must be measured (E3); do not assume datagram-like behaviour from a message API with internal queues.

Support [S15](https://caniuse.com/rtcpeerconnection): Chrome 56+ (data-channel capable line from 23+), Edge 79+, Firefox 22+/44+, Safari 11+ including iOS 11+; global 97.26%. WebRTC reaches more installed browsers than WebTransport today, and TURN gives it the best hostile-NAT story. Its tax is complexity and setup latency, not reach.

### WebSockets as fallback

The WebSocket Protocol is RFC 6455 [S16](https://datatracker.ietf.org/doc/html/rfc6455), bootstrappable over HTTP/2 (RFC 8441 lineage) and HTTP/3 (RFC 9220) [S17](https://datatracker.ietf.org/doc/html/rfc9220). MDN's WebSocket API page was opened [S18](https://developer.mozilla.org/en-US/docs/Web/API/WebSocket). caniuse: 97.49% global, effectively universal including old Safari/iOS [S19](https://caniuse.com/websockets).

The defect is structural: one ordered reliable TCP byte stream. One lost segment retransmits and every later byte waits (head-of-line blocking); Nagle coalescing can add sender-side delay for small writes unless the stack disables it, and the browser API gives the page no Nagle switch. Mitigations, all partial (**OPINION**): (a) send only the newest state per tick and drop stale queued data at the application layer by never queueing it — you cannot retract bytes already handed to the socket; (b) split traffic across multiple WebSocket connections by urgency so a loss on the bulk connection does not stall the urgent one — multiplies server state and only narrows, never removes, blocking; (c) run the RocketNet reliable channel (§ docs/CHANNELS.md layers) over it only for data that wants reliability anyway, and keep fast state on another carrier. None restores unreliable delivery. WebSocket is therefore a reach fallback and a signalling/lobby carrier, not a gameplay carrier for fast titles. Quantifying the percentile damage is E4.

### Anything else

WebSocket-over-HTTP/3 (RFC 9220 [S17](https://datatracker.ietf.org/doc/html/rfc9220)) removes TCP head-of-line across streams only if each game message gets its own QUIC stream — at which point WebTransport streams/datagrams are the more direct API on the same wire. Media-over-QUIC style relays and WebRTC-through-a-relay (TURN) are deployment shapes of the carriers above, not new browser APIs; TURN-relayed WebRTC is covered as the hostile-network arm of E5. A browser plugin is out per the brief. No other generally available browser datagram API was found in the status pages opened; claiming one would be **UNVERIFIED**.

## 2 Server side

The server stays native. Two shapes per carrier: embed the carrier stack in (or beside) the game server process, or run a gateway that terminates the browser carrier and speaks ordinary RocketNet UDP to the unmodified server. Library facts (licence, language, last push) were read 2026-10-14 from each repository via the GitHub API; scale figures not on a repository page are marked UNVERIFIED rather than recalled.

| Library | Carrier | Licence | Language | Last push when read | Shape for RocketNet |
|---|---|---|---|---|---|
| libdatachannel | WebRTC + WebSocket | MPL-2.0 | C++ | 2026-10-07 | Embed or gateway; closest C++ fit |
| Pion WebRTC | WebRTC | MIT | Go | 2026-10-14 | Gateway process (Go beside C++) |
| webrtc-rs | WebRTC | Apache-2.0 / MIT dual listed by repo | Rust | 2026-10-11 | Gateway process (Rust) |
| libwebrtc (native) | WebRTC | BSD-style (repo terms) UNVERIFIED detail | C++ | — | Embed; heaviest build |
| msquic | QUIC / HTTP/3 base | MIT | C | 2026-10-13 | Embed; needs HTTP/3 + WebTransport layer above |
| quiche | QUIC + HTTP/3 | BSD-2-Clause | Rust | 2026-10-13 | Gateway or FFI embed |
| ngtcp2 | QUIC (+ nghttp3) | MIT | C | 2026-10-11 | Embed; C fit, more assembly |
| lsquic | QUIC + HTTP/3 | MIT | C | 2026-10-12 | Embed |
| s2n-quic | QUIC | Apache-2.0 | Rust | 2026-10-14 | Gateway or FFI embed |
| uWebSockets | WebSocket | Apache-2.0 | C++ | 2026-10-05 | Embed or gateway; lightest fallback |

Repositories opened: libdatachannel https://github.com/paullouisageneau/libdatachannel, Pion https://github.com/pion/webrtc, webrtc-rs https://github.com/webrtc-rs/webrtc, msquic https://github.com/microsoft/msquic, quiche https://github.com/cloudflare/quiche, ngtcp2 https://github.com/ngtcp2/ngtcp2, lsquic https://github.com/litespeedtech/lsquic, s2n-quic https://github.com/aws/s2n-quic, uWebSockets https://github.com/uNetworking/uWebSockets, libsodium https://github.com/jedisct1/libsodium, Emscripten https://github.com/emscripten-core/emscripten.

Build burden on a C++ codebase with vendored libsodium (**OPINION**, from language/licence facts above): libdatachannel, msquic+WebTransport layer, ngtcp2, lsquic and uWebSockets keep one native toolchain; Pion/webrtc-rs/quiche/s2n-quic imply a second-language gateway process (operationally fine, arguably cleaner isolation) or FFI pain. Licence caution (**SOURCED** from the table): MPL-2.0 (libdatachannel) is file-level copyleft — usable in a commercial native server with care; the repo's own licence file governs. RocketNet's repo licence is AGPL-3.0 (README/LICENSE in the checkout) — gateway separation also keeps licence boundaries legible. Known scale figures (packets/sec/core, clients/core) for these stacks in game-datagram workloads are **UNVERIFIED**; vendor media benchmarks do not transfer. E6 exists to produce our own.

Gateway identity rule: STANDARD.md keys a connection by source IP+port. The gateway must therefore allocate one distinct UDP source port per browser session toward the game server and hold it for the session's life; NAT-style port sharing would merge two players into one apparent client and corrupt replay/ack state. ESTIMATED cost: one UDP socket (or bound port) per session on the gateway — thousands of sessions per gateway address are fine (port space 16 bits per destination), beyond that add gateway IPs. A gateway that instead terminates RocketNet crypto could multiplex freely, but §3 argues it must not.

Latency/CPU/memory: each gateway hop adds one UDP forwarding pass. ESTIMATED added latency on a LAN/co-located gateway: sub-millisecond plus scheduling; over a WAN-placed gateway it adds a full network leg, so gateways belong in the same facility as servers. CPU per client and memory per client are **UNVERIFIED** until E6; the design budget (**OPINION**) is that a ciphertext-relay gateway (no AEAD, no SCTP/QUIC termination beyond the browser leg) should be an order of magnitude cheaper per client than the game server itself — if E6 refutes that, embedding wins.

Fast path (XDP/GSO): DRIVERS.md's staged drivers (XDP, GSO) accelerate native UDP server sockets. Browser traffic arriving via a same-host gateway still enters the server as ordinary UDP, so the fast path survives for the server leg; browser-facing QUIC/WebRTC termination never touches XDP. If the carrier is embedded in-process instead, that traffic bypasses the driver fast path entirely. This asymmetry favours the gateway (**OPINION**).

Scaling and deployment: gateways are stateless apart from session→port mapping and carrier state; scale horizontally behind a load balancer with session affinity to the gateway that owns the carrier connection (QUIC connection migration can move networks but not gateway processes mid-connection without state transfer — UNVERIFIED for browser WebTransport migration in practice; E5 probes). TLS: WebTransport/WebSocket gateways need certificates per public hostname (automated issuance; §7); WebRTC needs DTLS certificates (self-signed fingerprints via SDP are normal) plus TURN credentials. The native server's existing deployment is untouched in the gateway shape.

## 3 Protocol mapping

Decide once, apply per carrier: **keep RocketNet encryption end-to-end** (**OPINION**, argued here). The browser client and the native server run the unmodified token/handshake/AEAD/replay design; the gateway relays sealed RocketNet packets as opaque payloads inside the carrier. The gateway then cannot read or forge game data, needs no session keys, and its compromise exposes metadata (timing, sizes, endpoints), not plaintext. The alternative — terminate RocketNet at the gateway and trust DTLS/QUIC alone — saves the double encryption costed below but makes the gateway a plaintext trusted party, forces a STANDARD.md fork for browser clients, and destroys the property that a browser session is indistinguishable from a native one at the server. That saving does not buy its trust.

Byte cost (ESTIMATED, arithmetic shown): RocketNet sealed overhead is 26 bytes (6 prefilter + 4 sequence + 16 tag) plus ≥7-byte ack block on session packets (STANDARD.md, Packet sizes). Inside WebTransport datagrams add QUIC short-header + datagram frame overhead (single-digit to ~20 bytes, path dependent, UNVERIFIED exact). Inside WebRTC add SCTP/DTLS/UDP/IP overhead (tens of bytes; DTLS record overhead alone is of the same order as RocketNet's tag). Inside wss add WS framing (2–14 bytes) + TLS/TCP/IP. Consequence: the 1232 budget must shrink by the carrier overhead on each path; E2/E3 measure the true maxima. CPU cost of double AEAD: one extra ChaCha20-Poly1305 pass per packet in browser WASM and on the server — small next to carrier stacks, and it is the price of end-to-end trust; E6 measures it rather than asserting it.

Token delivery: the 768-byte token (1024 base64 chars) reaches the page over HTTPS from the game backend exactly as native clients receive it (STANDARD.md, Connect Token). Recommended (**OPINION**): fetch via `fetch()` from JS/WASM and hand bytes to the client core; never place the token in a URL fragment (logged, leaked via history/referrer) and never in a cookie (CSRF surface, size limits). The gateway checks nothing cryptographic: there is deliberately no shared secret (STANDARD.md, Keys). The first RocketNet connection-request packet travels inside the carrier to the server, which verifies Ed25519 signature and opens the sealed block as today. A gateway may apply only cheap pre-filtering (rate limits, size checks); it must not invent a token check it cannot perform.

Identity and handshake mapping: gateway-assigned per-session UDP source ports (§2) preserve the source-address identity rule unchanged; the server cannot tell a gateway session from a native client. The cookie exchange: QUIC and DTLS already validate return routability (QUIC path validation / ICE consent + DTLS handshake), so the RocketNet cookie round trip is partly redundant as address proof on browser paths — but it also paces server allocation (nothing allocated before cookie return) and its cost is one small round trip inside an already-established carrier. Recommendation (**OPINION**): keep the handshake byte-identical on all carriers (no STANDARD.md change, one conformance story); revisit only if E1 shows setup latency dominated by it. Total setup = carrier setup (§1) + 3 RocketNet round trips over that carrier (STANDARD.md, The Handshake); on WebTransport that is QUIC setup + 3 local legs, on WebRTC it stacks on the slowest setup — another reason WebRTC is fallback, not primary.

Fragmentation: RocketNet fragments large payloads in-transport (type 9) under its 1232 budget; carriers must carry each RocketNet packet whole. If a carrier's datagram maximum is below the packet size, the client clamps its send budget (§1) so fragmentation still happens at the RocketNet layer, never by splitting one RocketNet packet across carrier messages (which would break ack/replay atomicity, cf. CHANNELS.md P2/P3).

Migration: mobile browsers change networks. Native RocketNet loses the session on address change (STANDARD.md is explicit) and reconnects with a fresh token. QUIC offers connection migration, but a gateway-fronted browser session migrating would change the gateway→server UDP source address unless the gateway anchors it — with per-session gateway ports, migration can be absorbed at the gateway and the server sees no change: a genuine browser-path advantage worth testing (E5), not a promise.

STANDARD.md additions needed (**OPINION**): none for the gateway design. A future native-migration packet type (already contemplated by STANDARD.md's reserved types) and a browser-carrier profile (budget clamping, token-over-HTTPS delivery) belong in a profile document, not the wire standard.

## 4 Client library

Two families. (a) Compile the existing C++ client to WebAssembly with Emscripten (repo opened: https://github.com/emscripten-core/emscripten) behind a JavaScript transport adapter; the C API in sdk/rocketnet.h is the seam, and DRIVERS.md's driver table gains a JS driver that takes sealed packets from WASM and hands them to the carrier API, and vice versa. (b) A thin TypeScript re-implementation of the wire from STANDARD.md.

Crypto decides much of this. RocketNet needs ChaCha20-Poly1305 IETF AEAD, BLAKE2b KDF, X25519, Ed25519 verification-adjacent operations and SipHash (STANDARD.md, Keys/Constants). WebCrypto (MDN SubtleCrypto, opened [S20](https://developer.mozilla.org/en-US/docs/Web/API/SubtleCrypto)) does not offer ChaCha20-Poly1305 — that absence is long-standing and remains the blocker; Ed25519 in SubtleCrypto is now broad (Chrome/Edge 137+, Firefox 129+, Safari 17+; 89.82% when opened [S21](https://caniuse.com/mdn-api_subtlecrypto_sign_ed25519)) but one algorithm does not make a suite. Therefore both families need libsodium compiled to WASM: the libsodium.js build (npm page opened: complete library 188 KB minified+gzipped including JS+WASM [S22](https://www.npmjs.com/package/libsodium)) or a trimmed Emscripten build of only the needed primitives. ESTIMATED sizes: WASM client core (transport + channels + trimmed libsodium) in the low hundreds of KB gzipped; TypeScript client plus the same libsodium WASM saves the C++ runtime but not the crypto. Exact figures are measurement tasks (E7), stated here as ESTIMATED only. WebAssembly support itself is 97.24% [S23](https://caniuse.com/wasm).

Threading: SharedArrayBuffer requires cross-origin isolation (COOP/COEP headers), which breaks common ad/analytics embeds and some engine export defaults. caniuse SharedArrayBuffer: 96.94% where isolation is deployed [S24](https://caniuse.com/sharedarraybuffer). Design (**OPINION**): single-threaded WASM client driven from a Web Worker, packets via `postMessage`/datagram streams; require no SharedArrayBuffer for v1. AudioWorklet is irrelevant to the transport; OffscreenCanvas matters to engines, not to this library.

Engines and bindings: the existing C# binding (csharp/ in the checkout) is the Unity path natively; Unity WebGL and Blazor cannot load that native binding in-browser and need the WASM build exposed to JS, then to the engine (Unity WebGL via a `.jslib` bridge; Godot via its JavaScript bridge; Bevy via wasm-bindgen-style JS glue; plain canvas via the JS API directly). Engine-specific bridge costs are **UNVERIFIED**; E8 prototypes the cheapest (plain canvas + Unity WebGL). Conformance: STANDARD.md Appendix A known-answer vectors must run in the browser build in CI (headless browser), for both WASM and any TypeScript implementation; CHANNELS.md's socketless unit-test rule (no real time/sockets in unit tests) applies unchanged.

Verdict (**OPINION**): family (a) first — one wire implementation, vectors already exist, drift is impossible by construction. Family (b) only if E7 shows WASM size/startup materially worse for page-embedded games.

## 5 Measurement plan

Nothing is run on this card. Harness: one native server host (Linux, portable driver first, then GSO/XDP where available), one gateway host co-located, browser clients on (i) desktop Mac, (ii) Windows desktop, (iii) Android phone, (iv) iPhone. Browsers: current stable Chrome, Edge, Firefox, Safari (versions recorded at run time). Timestamps: in-page `performance.now()` around send/receive plus server-side packet timestamps correlated by sequence number; one-way latency via synchronized clocks is UNVERIFIED methodology — report round-trip/2 and jitter (p50/p95/p99/p99.9 of inter-arrival deviation) unless PTP sync is available. Sample size: ≥100k packets per cell for p99.9 claims.

Matrix per carrier (WebTransport datagrams, WebRTC unreliable, wss): send rates 10/30/60 pps; payloads 100/400/1200 bytes; netem profiles: loss 0/1/5/10%, added delay 20/50/100 ms, jitter 10% of delay, reorder 1%, rate caps 1/5/20 Mbit/s, burst loss (Gilbert model). Metrics: one-way latency percentiles, jitter, goodput, setup time (cold/warm), server pps/core and bytes/client memory (gateway and server separately), browser CPU % and phone battery drain per 10 min, NAT matrix (full-cone, restricted, symmetric, CGNAT, IPv6-only with NAT64) and cellular runs, HTTP/3-blocked and UDP-blocked network arms.

Pass lines (decision rules, **OPINION**): primary carrier wins if its p99 added latency vs native UDP baseline is ≤5 ms at 60 Hz/400 B under 1% loss and setup ≤500 ms warm; WebSocket is disqualified as gameplay carrier if its p99 delay under 1% loss exceeds the datagram carrier's by ≥20 ms (expected); gateway is viable if it sustains ≥200k pps/core relaying ciphertext (target to be reset by E6's first numbers). These lines are proposals for the experiment cards, not results.

## 6 Support matrix

Read 2026-10-14 from caniuse pages cited; versions are first supported versions. Shares: WebTransport 91.64% [S6](https://caniuse.com/webtransport), WebRTC 97.26% [S15](https://caniuse.com/rtcpeerconnection), WebSocket 97.49% [S19](https://caniuse.com/websockets), WASM 97.24% [S23](https://caniuse.com/wasm), HTTP/3 94.55% [S7](https://caniuse.com/http3).

| Browser / platform | WebTransport | WebRTC data | WebSocket | Note |
|---|---|---|---|---|
| Chrome desktop (Win/mac/Linux) 97+ | Yes | Yes (56+) | Yes (16+) | Primary path |
| Edge desktop 98+ | Yes | Yes (79+) | Yes | Chromium engine |
| Firefox desktop 114+ | Yes | Yes (44+) | Yes (11+) | — |
| Safari macOS 26.4+ | Yes | Yes (11+) | Yes (7+) | Pre-26.4 Safari: no WebTransport |
| Chrome Android (current) | Yes | Yes | Yes | Version tracks Chrome |
| Safari iOS/iPadOS 26.4+ | Yes | Yes (11+) | Yes (6+) | All iOS browsers are WebKit: Chrome/Firefox on iOS inherit this row |
| Chrome/Firefox iOS | As Safari iOS | As Safari iOS | As Safari iOS | Engine is WebKit regardless of brand |
| Samsung Internet 18+ | Yes | Yes | Yes | Chromium-based |
| Opera 83+ | Yes | Yes (43+) | Yes | Chromium-based |
| Electron / Steam overlay webview | Chromium-dependent UNVERIFIED | Yes typically | Yes | Pin/test per app |
| Capacitor / Unity WebGL in iOS app | WebKit row | WebKit row | WebKit row | WKWebView limits apply UNVERIFIED detail |

Enterprise/school networks (**OPINION** with UNVERIFIED rates): UDP egress blocked → WebTransport and WebRTC-UDP fail, TURN-over-TLS/443 or wss survives; HTTP/3 (UDP 443) blocked but UDP otherwise open → WebRTC survives, WebTransport fails; TLS inspection → wss/WSS and WebTransport certificates must be publicly trusted; WebRTC disabled by policy → datagram primary only. No trustworthy public percentage per policy was found: UNVERIFIED; E5's field test must sample real player networks rather than assume.

## 7 Ideas of your own

- Idea: happy-eyeballs transport racing — start WebTransport, WebRTC and wss in parallel with staggered starts (0/150/400 ms), first carrier to complete the RocketNet handshake wins, losers close. Hides every carrier's worst setup tail.
- Idea: one gateway binary, three front doors — the same per-session UDP relay behind WebTransport, WebRTC and wss listeners, so fallback changes the browser leg only and server operations never fork.
- Idea: token delivery through the page's existing authenticated `fetch()` (never URL/cookie; §3), with the backend minting per-carrier-identical tokens — the token does not name a carrier.
- Idea: spectator mode over WebTransport streams — reliable streams on the same QUIC connection carry replay/spectator data while datagrams carry play.
- Idea: replays in the browser — the WASM client plus recorded payload streams gives free replay playback in-page for support and anti-cheat review.
- Idea: browser bot/anti-cheat posture — browsers are inspectable; assume client memory is public, keep authority on the server, treat token theft (Bearer [REDACTED], STANDARD.md) as the main browser-specific risk (XSS exfiltrates tokens: short expiries, connected-set enforcement).
- Idea: certificate automation for community servers — ACME-issued certificates for gateway hostnames so self-hosted servers can accept WebTransport without `serverCertificateHashes` gymnastics.
- Idea: WebGPU/WebGL titles sharing the page — run the client in a Worker so rendering jank never blocks packet pumps; define a frame-boundary delivery contract like CHANNELS.md layer 8.
- Idea: connection-migration as a browser selling point — gateway-anchored migration (§3) could make browser clients survive network switches better than native clients do today.

## 8 Ranked R&D experiments

Ranked by (value to the primary/fallback/gateway decision) ÷ cost. Costs are worker-days (ESTIMATED). At least E5 and E9 can overturn this document's recommendation.

| E1 | Browser page ↔ native server on a Mac | Q: can a page exchange RocketNet packets with the native server at all, end to end? H: WebTransport datagrams + ciphertext-relay gateway + WASM client completes handshake and 60 s of payload exchange. Build: minimal gateway (msquic or quiche WebTransport listener → per-session UDP port), Emscripten client core with JS driver, one test page. Measure: handshake success, echo latency p50/p99. Cost: 5 d. Dead-end: no WebTransport stack interoperates with stable Chrome+Safari from a native server after 2 attempts → primary falls to WebRTC. Unblocks: everything. From §1/§2. |
| E2 | True datagram ceilings | Q: max usable datagram/packet size per browser per carrier? H: ≥1232−overhead fits on WebTransport in all four browsers. Build: size-sweep probe page + gateway counters. Cost: 2 d. Dead-end: any primary browser caps below RocketNet's minimum viable budget → budget-profile redesign before any client work. Unblocks: §3 budget clamp. From §1/§3. |
| E3 | WebRTC unreliable behaviour at 60 Hz | Q: do `maxRetransmits:0` channels stay timely under loss, or buffer? H: p99 delay within 10 ms of UDP baseline at 5% loss. Build: libdatachannel gateway + test page. Cost: 4 d. Dead-end: SCTP buffering adds >25 ms p99 → WebRTC demoted below wss for fast play. Unblocks: fallback choice. From §1. |
| E4 | WebSocket HOL quantification | Q: how bad is wss under loss, exactly? H: p99 penalty ≥20 ms at 1% loss (confirming disqualification). Build: uWebSockets gateway arm in the E1 harness. Cost: 2 d. Dead-end (for this document's ranking): penalty <5 ms → wss-first simplicity wins and gateway scope shrinks. Unblocks: fallback scope. From §1/§5. |
| E5 | Hostile-network field matrix | Q: which carriers connect from real player networks (UDP-blocked, H3-blocked, symmetric NAT, cellular)? H: WebTransport ≥85% connect success, WebRTC+TURN ≥95%. Build: connect-probe page + TURN arm, recruit sample. Cost: 6 d. Dead-end/overturn: WebTransport success < WebRTC by ≥15 points → swap primary/fallback. Unblocks: launch gating. From §1/§6. |
| E6 | Gateway cost at load | Q: pps/core, CPU/client, memory/client for ciphertext relay vs embedded termination? H: relay ≥200k pps/core. Build: load generator driving gateway + server. Cost: 5 d. Dead-end: relay <50k pps/core → evaluate embedding despite §2. Unblocks: server architecture. From §2/§3. |
| E7 | WASM vs TypeScript client bake-off | Q: size, startup, per-packet CPU of Emscripten C++ vs TS+libsodium-WASM? H: WASM within 20% of TS on all three, with zero wire drift. Build: both clients against Appendix A vectors. Cost: 8 d. Dead-end: WASM startup >2× TS on mid phone → TS becomes primary client. Unblocks: §4. From §4. |
| E8 | Engine bridges | Q: Unity WebGL + plain canvas (+ Godot if cheap) consume the client at 60 Hz? H: bridge overhead <1 ms/frame. Build: `.jslib` + canvas demos. Cost: 6 d. Dead-end: Unity bridge cannot sustain rate → engine-specific native-plugin strategy needed. Unblocks: adoption. From §4. |
| E9 | Migration reality check | Q: does a browser session survive Wi-Fi→cellular with gateway anchoring? H: no server-visible disconnect. Build: E1 harness + network-switch script. Cost: 3 d. Overturn: if migration works natively in WebTransport without gateway help, gateway value drops and embedded WebTransport rises. Unblocks: §3 migration claim. From §3. |
| E10 | Setup-latency shootout | Q: cold/warm connect time per carrier incl. RocketNet 3-RTT handshake? H: WebTransport warm ≤300 ms to first payload. Build: timing instrumentation in E1/E3/E4 arms. Cost: 2 d (incremental). Dead-end: handshake dominates → propose STANDARD profile collapsing cookie step on validated carriers. Unblocks: UX targets. From §1/§3. |
| E11 | serverCertificateHashes operability | Q: can self-hosted servers run WebTransport with hash-pinned certs across browsers? H: works in Chrome+Firefox, fails Safari (to verify). Build: hash-rotation test server. Cost: 3 d. Dead-end: any primary browser rejects rotation flow → ACME-only policy. Unblocks: community servers. From §1/§7. |
| E12 | TURN cost and abuse model | Q: what does TURN-relayed fallback cost per player-hour and how is it credentialed? H: relay bandwidth cost acceptable at ≤5% of sessions. Build: TURN deployment + metering in E5. Cost: 4 d. Dead-end: relay share >20% of sessions → renegotiate fallback economics. Unblocks: ops budget. From §1/§2. |

Monday start for E1 (first rung): a worker takes the staged checkout, builds the native server on a Mac with the portable driver, stands up the smallest WebTransport gateway from §2's C/C++ options, compiles the client core with Emscripten behind a JS driver stub, and opens one page that sends the connection request and logs the challenge/response/keep-alive exchange. Success is visible in the server log as an ordinary client on a gateway source port.

## 9 Sources

All opened 2026-10-14. S-numbers are cited inline above; repository licence/language/activity came from each repo via the GitHub API on the same date.

- S1 WebTransport W3C Candidate Recommendation — API, datagrams, streams, certificate options. https://www.w3.org/TR/webtransport/
- S2 QUIC transport, RFC 9000 — handshake, datagrams, congestion control. https://datatracker.ietf.org/doc/html/rfc9000
- S3 HTTP/3, RFC 9114 — WebTransport's HTTP layer. https://datatracker.ietf.org/doc/html/rfc9114
- S4 MDN WebTransport constructor — options including certificate hashes. https://developer.mozilla.org/en-US/docs/Web/API/WebTransport/WebTransport
- S5 MDN WebTransport API overview. https://developer.mozilla.org/en-US/docs/Web/API/WebTransport
- S6 caniuse WebTransport — versions and 91.64% share. https://caniuse.com/webtransport
- S7 caniuse HTTP/3 — versions and 94.55% share. https://caniuse.com/http3
- S8 WebRTC W3C Recommendation. https://www.w3.org/TR/webrtc/
- S9 WebRTC Data Channels, RFC 8831 — SCTP over DTLS. https://datatracker.ietf.org/doc/html/rfc8831
- S10 ICE, RFC 8445 — NAT traversal. https://datatracker.ietf.org/doc/html/rfc8445
- S11 TURN, RFC 8656 — relay traversal. https://datatracker.ietf.org/doc/html/rfc8656
- S12 WebRTC Security Architecture, RFC 8827. https://datatracker.ietf.org/doc/html/rfc8827
- S13 SDP bundle negotiation, RFC 9143. https://datatracker.ietf.org/doc/html/rfc9143
- S14 MDN createDataChannel — ordered/maxRetransmits/maxPacketLifeTime. https://developer.mozilla.org/en-US/docs/Web/API/RTCPeerConnection/createDataChannel
- S15 caniuse WebRTC peer connections — versions and 97.26% share. https://caniuse.com/rtcpeerconnection
- S16 The WebSocket Protocol, RFC 6455. https://datatracker.ietf.org/doc/html/rfc6455
- S17 Bootstrapping WebSockets with HTTP/3, RFC 9220. https://datatracker.ietf.org/doc/html/rfc9220
- S18 MDN WebSocket API. https://developer.mozilla.org/en-US/docs/Web/API/WebSocket
- S19 caniuse Web Sockets — versions and 97.49% share. https://caniuse.com/websockets
- S20 MDN SubtleCrypto — algorithm surface (no ChaCha20-Poly1305). https://developer.mozilla.org/en-US/docs/Web/API/SubtleCrypto
- S21 caniuse SubtleCrypto Ed25519 — versions and 89.82% share. https://caniuse.com/mdn-api_subtlecrypto_sign_ed25519
- S22 libsodium.js npm — 188 KB minified+gzipped complete build. https://www.npmjs.com/package/libsodium
- S23 caniuse WebAssembly — versions and 97.24% share. https://caniuse.com/wasm
- S24 caniuse SharedArrayBuffer — versions and 96.94% share. https://caniuse.com/sharedarraybuffer
- S25 Chrome WebTransport developer guide. https://developer.chrome.com/docs/capabilities/web-apis/webtransport
- S26 libdatachannel repository — MPL-2.0, C++, WebRTC/WebSocket. https://github.com/paullouisageneau/libdatachannel
- S27 Pion WebRTC repository — MIT, Go. https://github.com/pion/webrtc
- S28 webrtc-rs repository — Rust WebRTC. https://github.com/webrtc-rs/webrtc
- S29 msquic repository — MIT, C QUIC. https://github.com/microsoft/msquic
- S30 quiche repository — BSD-2-Clause, Rust QUIC/HTTP/3. https://github.com/cloudflare/quiche
- S31 ngtcp2 repository — MIT, C QUIC. https://github.com/ngtcp2/ngtcp2
- S32 lsquic repository — MIT, C QUIC/HTTP/3. https://github.com/litespeedtech/lsquic
- S33 s2n-quic repository — Apache-2.0, Rust QUIC. https://github.com/aws/s2n-quic
- S34 uWebSockets repository — Apache-2.0, C++. https://github.com/uNetworking/uWebSockets
- S35 libsodium repository — crypto library used by RocketNet. https://github.com/jedisct1/libsodium
- S36 Emscripten repository — LLVM-to-WebAssembly toolchain. https://github.com/emscripten-core/emscripten
- S37 WHATWG HTML (WebSocket living-standard reference from caniuse). https://html.spec.whatwg.org/multipage/comms.html
- S38 WebRTC explainer/status via caniuse resources (WebRTC project). https://webrtc.org/
- S39 MDN RTCPeerConnection reference (API family). https://developer.mozilla.org/en-US/docs/Web/API/RTCPeerConnection
- S40 caniuse usage-table methodology (StatCounter shares). https://caniuse.com/usage-table

Local sources (no URL): STANDARD.md, docs/CHANNELS.md, sdk/rocketnet.h, DRIVERS.md, README.md in the staged checkout at 11a39bf.

## 10 Not done

- No code, build, benchmark or browser run was performed; every performance figure is ESTIMATED or UNVERIFIED and routed to §8 experiments.
- Exact per-browser WebTransport datagram maxima, WebRTC message caps and QUIC pacing behaviour at 60 Hz were not settled from documents; E2/E3/E10 own them.
- libwebrtc native build cost, Electron/Steam/Capacitor webview specifics, enterprise-network blocking rates and TURN pricing were not sourced deeply enough; marked UNVERIFIED where they appear.
- Firefox/Safari WebTransport interop with each native QUIC stack (as opposed to API presence) is untested; E1 is designed to fail loudly on exactly this.
- Next: run E1, then E2/E4 in its harness; revisit the primary/fallback ranking with E5 field data before committing to a client family beyond the E7 bake-off.
