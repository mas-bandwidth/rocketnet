# RocketNet in the Web Browser: Transport, Gateway, Protocol Mapping, and Client Library Research

*Research date: 2026-10-14. All external specifications, browser compatibility datasets, and library repositories cited in this document were opened and read on 2026-10-14 (see `## 9 Sources` for `[S1]`–`[S75]`). Statements are labeled `[SOURCED: S#]`, `[ESTIMATED: arithmetic]`, `[OPINION]`, or `[UNVERIFIED]`.*

**Executive Summary (`[OPINION]` grounded in `[SOURCED]` facts):**
1. **Primary & Fallback Transport Architecture:** No single browser transport reaches 100% of players on every network with unreliable unordered datagrams. **WebTransport over HTTP/3 (QUIC datagrams, RFC 9221 / RFC 9297)** is the cleanest technical match for RocketNet (1-RTT connection setup, native unreliable unordered datagrams, zero SCTP/ICE/STUN overhead, and support for ephemeral self-signed server certificates via `serverCertificateHashes` up to 14 days `[SOURCED: S1, S16]`), and as of Safari 26.4 (March 2026) it has finally reached all three major browser engines (`Chrome 97+`, `Edge 98+`, `Firefox 114+`, `Safari 26.4+` on macOS and iOS `[SOURCED: S9, S54, S60]`). However, pre-26.4 iOS/macOS devices, PlayCanvas/iOS WebView wrappers, and UDP-throttling or HTTP/3-blocking networks still require **WebRTC `RTCDataChannel`** (`ordered: false, maxRetransmits: 0`, baseline across Chrome 24+, Firefox 22+, Safari 11+ `[SOURCED: S10, S18, S56]`) and **WebSockets over TLS (TCP)** as the universal last-resort fallback (`[SOURCED: S11, S58]`). A browser client should race **WebTransport $\rightarrow$ WebRTC (ICE-lite) $\rightarrow$ WebSocket** in a Happy Eyeballs cascade (`[OPINION]`).
2. **Server Side (Sidecar/Co-located Gateway vs. In-Process Embedding):** Keep the native C++ RocketNet server binary completely untouched on its UDP fast path (`XDP`, `GSO`, `sendmmsg`/`recvmmsg`, `sendmsg_x`/`recvmsg_x` `[SOURCED: DRIVERS.md]`) and run a **co-located native gateway process** (or sidecar thread/daemon on the same host over loopback `127.0.0.1` / `::1`) that terminates the outer browser transport (HTTP/3 WebTransport, DTLS/SCTP WebRTC, or TLS/TCP WebSocket), allocates **one ephemeral local UDP socket per browser session** (`bind(127.0.0.1:0)` + `connect(server_ip:server_port)`), and forwards raw RocketNet UDP datagrams verbatim (`[OPINION]`). On Linux loopback, one UDP hop adds ~8–25 $\mu\text{s}$ of latency (`[ESTIMATED]`), preserves the server's exact `"client address is the connection identity"` invariant (`STANDARD.md:348`), keeps OpenSSL/BoringSSL/Rust/Go out of the core C++ library (`sodium/sodium.c` remains the sole cryptographic dependency of `rocketnet`), and allows native UDP clients to keep using the XDP/GSO fast path on the external NIC (`[OPINION]`).
3. **End-to-End Encryption vs. Gateway Termination:** **Keep RocketNet's ChaCha20-Poly1305 AEAD, sequence numbers, 4096-packet replay window, and 3-RTT handshake end-to-end between the browser client and the native RocketNet server** (`[OPINION]`). The 26-byte RocketNet wire header + tag (plus 7-byte minimum ack block) costs only **2.68% of a 1232-byte packet** (`33 / 1232 = 2.68%` `[ESTIMATED]`) and ~1.5–4 $\mu\text{s}$ of browser CPU per packet at 60 Hz (`< 0.03%` of a 16.67 ms frame `[ESTIMATED]`). In exchange, the gateway is an **untrusted ciphertext relay**: it needs zero shared secrets with the game server, cannot read or forge game payloads, and requires **zero changes to `STANDARD.md` or the native server** (`[SOURCED: STANDARD.md:755-893, 989-1283]`).
4. **Client Library:** Build **both** (a) an **Emscripten WebAssembly build of the existing C++ client** (`sdk/rocketnet_client_transport.cpp` + `sdk/rocketnet_client_protocol.cpp` + `sodium/sodium.c` behind a custom `rocketnet_client_driver_operations_t` JS ring-buffer seam, `[SOURCED: sdk/rocketnet_driver.h:76-119]`) as the reference implementation and for C#/Unity WebGL/WASM engines, and (b) a **zero-dependency TypeScript + `libsodium.js` (WASM crypto) client** (~15–25 KB minified JS + ~95 KB gzipped WASM libsodium `[ESTIMATED]`) for weight-sensitive web games (`[OPINION]`). One surgical change to `sdk/rocketnet_client_transport.cpp` is required for WASM: allow single-threaded polling inside `rocketnet_client_transport_update()` when compiled with `__EMSCRIPTEN__` without `-pthread`, eliminating the hard requirement for `SharedArrayBuffer` and COOP/COEP headers (`[SOURCED: sdk/rocketnet_client_transport.cpp:355-369, S27, S59]`).

---

## 1 Transports

### 1.1 Transport Mechanisms and Specification Ground Truth (Read 2026-10-14)

#### A. WebTransport over HTTP/3 (`WebTransport` API)
* **Protocol stack:** Runs over QUIC v1 (`[SOURCED: S35, RFC 9000]`) and HTTP/3 (`[SOURCED: S37, RFC 9114]`) using Extended CONNECT (`[SOURCED: S33, draft-ietf-webtrans-http3]`), multiplexing both **unreliable unordered QUIC `DATAGRAM` frames** (`[SOURCED: S5, RFC 9221]`) encapsulated as HTTP Datagrams with a Quarter-Stream-ID context varint (`[SOURCED: S4, RFC 9297]`) and **reliable QUIC unidirectional/bidirectional streams** (`[SOURCED: S1, W3C WebTransport CR]`).
* **Browser status today (read 2026-10-14):**
  * **Chrome Desktop & Android:** Shipped in **Chrome 97** (Desktop) and supported in current **Chrome Android 154** (`[SOURCED: S9, S54, S60]`).
  * **Edge Desktop:** Shipped in **Edge 98** (`[SOURCED: S9, S60]`).
  * **Firefox Desktop & Android:** Shipped in **Firefox 114** (Desktop) and supported in **Firefox Android 157** (`[SOURCED: S9, S54, S60]`).
  * **Safari (macOS & iOS/iPadOS):** **Shipped in Safari 26.4 and iOS Safari 26.4** (achieving W3C Baseline `low` / Newly Available status on `2026-03-24`; unsupported in Safari $\le 26.3$ and iOS $\le 26.3$ `[SOURCED: S9, S54, S60]`).
  * **Critical cross-browser API divergence discovered in live BCD data (`2026-10-14`):** The W3C WebTransport Candidate Recommendation (`[SOURCED: S1, §5.2]`) replaced the legacy `datagrams.writable` attribute with the `datagrams.createWritable(options)` method. According to MDN Browser Compat Data (`[SOURCED: S55]`), **Safari 26.4 implements `datagrams.createWritable()` (`26.4`) and sets `datagrams.writable` to `false`**, whereas **Chrome (`97`–`157`) implements `datagrams.writable` and sets `datagrams.createWritable()` to `false`** (Firefox `155+` supports both). As documented in the W3C specification (`[SOURCED: S1, §15.10]`), a browser client MUST normalize the writer with:
    ```javascript
    const writable = wt.datagrams.createWritable
      ? wt.datagrams.createWritable()
      : wt.datagrams.writable;
    ```
* **Certificate handling and `serverCertificateHashes` limits:**
  * WebTransport mandates TLS 1.3 over QUIC (`https://` URL with explicit port `[SOURCED: S1, S16]`).
  * Standard connections require a Web PKI certificate chaining to a browser-trusted root CA (e.g., via Let's Encrypt ACME `[SOURCED: S52, RFC 8555]`).
  * For bare-IP or ephemeral dedicated game servers that do not have a DNS hostname or CA-signed certificate, `new WebTransport(url, { serverCertificateHashes: [{ algorithm: "sha-256", value: hashBytes }] })` allows pinning a raw SHA-256 certificate digest (`[SOURCED: S1, §6.9; S16]`).
  * **Strict limits on `serverCertificateHashes` (`[SOURCED: S1, §6.9 custom-certificate-requirements]`):**
    1. **Maximum validity period of 14 days (two weeks):** *"the current time MUST be within the validity period of the certificate as defined in Section 4.1.2.5 of [RFC5280] and the total length of the validity period MUST NOT exceed two weeks"* (`[SOURCED: S1, §6.9]`). Game servers or gateways must rotate ephemeral self-signed X.509v3 certificates at least every 14 days (in practice every 7–10 days) and report the current SHA-256 hash to the matchmaking/token backend.
    2. **Key algorithm restriction (no RSA):** The Subject Public Key Info field *"MUST include ECDSA with the secp256r1 (NIST P-256) named group ([RFC3279], Section 2.3.5; [RFC9846]) to provide an interoperable default. It MUST NOT contain RSA keys"* (`[SOURCED: S1, §6.9]`).
    3. **Dedicated connection only (`allowPooling: false`):** Passing `serverCertificateHashes` with `allowPooling: true` throws a `NotSupportedError` DOMException (`[SOURCED: S1, §6.2; S16]`).
    4. **Browser support for `serverCertificateHashes`:** Chrome 100+, Edge 100+, Firefox 125+, Safari 26.4+ (`[SOURCED: S54]`).
* **Datagram size limit (`maxDatagramSize`):** Exposed via `wt.datagrams.maxDatagramSize` (`[SOURCED: S1, §5.1; S17]`). QUIC v1 (`[SOURCED: S35, RFC 9000 §14]`) requires a minimum initial UDP payload of **1200 bytes**. After subtracting the QUIC 1-RTT short header (`1` byte flags + `0..20` bytes Destination Connection ID + `1..4` bytes packet number), the 16-byte TLS 1.3 AEAD tag, the 1-byte `DATAGRAM` frame type (`0x30`/`0x31`, `[SOURCED: S5, RFC 9221 §4]`), and the 1-byte HTTP/3 Quarter-Stream-ID (`0x00`, `[SOURCED: S4, RFC 9297 §2.1]`), the guaranteed minimum `maxDatagramSize` without QUIC Path MTU Discovery (PMTUD, RFC 8899) is **$1200 - (1 + 8 + 2 + 16 + 1 + 1) = 1171\text{ bytes}$** (`[ESTIMATED: 1200 - 29 = 1171]`). When PMTUD succeeds on a standard 1500-byte Ethernet path (or 1280+ byte IPv6 path), `maxDatagramSize` rises to **~1350–1420 bytes**, easily fitting RocketNet's **1232-byte** default send budget (`[ESTIMATED]`). However, before PMTUD completes (or on strict 1200-byte QUIC minimum paths), a 1232-byte RocketNet packet exceeds 1171 bytes (see `## 3 Protocol mapping` for exact mitigation).
* **Congestion control on 10–60 Hz game traffic:** QUIC `DATAGRAM` frames are not retransmitted on loss, **but they are strictly subject to the QUIC connection's congestion controller** (`[SOURCED: S5, RFC 9221 §5.2; S36, RFC 9002]`, typically CUBIC or BBR in browser stacks). Note that `WebTransportOptions.congestionControl` (`"default" | "throughput" | "low-latency"`) is only a non-binding hint and is **`false` (unimplemented) in Chrome 97–157** (`[SOURCED: S16, S54]`). At 10–60 Hz with 100–1200 B packets (8–576 kbps), a game send pattern sits well below normal broadband bandwidth and inside the initial 10-packet congestion window (`~12–14.6 KB`), so congestion control rarely throttles steady-state ticks. However, during a burst loss event or severe RTT spike, QUIC's pacing/cwnd can briefly queue outgoing datagrams in the browser's outgoing datagram buffer unless `wt.datagrams.outgoingMaxAge` (e.g., `50 ms`) or `outgoingHighWaterMark` (e.g., `4`–`8` datagrams) is configured to drop stale ticks (`[SOURCED: S1, §5.1; S17]`).

#### B. WebRTC Data Channels (`RTCPeerConnection` + `RTCDataChannel`)
* **Protocol stack:** Encapsulates user messages inside **SCTP** (`[SOURCED: S43, RFC 9260]`) with the **PR-SCTP** partial reliability extension (RFC 3758) and `I-DATA` message interleaving (`[SOURCED: S42, RFC 8260]`), running over **DTLS 1.2 / 1.3** (`[SOURCED: S44, RFC 6347; S45, RFC 9147]`), over **ICE / STUN / TURN** (`[SOURCED: S38, RFC 8445; S39, RFC 8489; S40, RFC 8656]`) over UDP (`[SOURCED: S6, RFC 8831; S7, RFC 8832]`).
* **Browser status today (read 2026-10-14):**
  * **W3C Baseline `high` (Widely Available since July 2022):** Supported in **Chrome 24+** (`ordered`, `maxRetransmits`, `negotiated` since Chrome 30; `maxPacketLifeTime` since Chrome 56), **Edge 79+**, **Firefox 22+** (`maxPacketLifeTime` / `maxRetransmits` since Firefox 62), **Safari 11+** (macOS) and **iOS Safari 11.0+**, **Chrome Android 154**, **Firefox Android 24+**, and **Android WebView** (`[SOURCED: S10, S18, S56, S61]`).
* **Unreliable and unordered mode (`ordered: false, maxRetransmits: 0`):**
  * Configured via `pc.createDataChannel("rocketnet", { ordered: false, maxRetransmits: 0, negotiated: true, id: 0 })` (`[SOURCED: S2, S6, S19]`).
  * Setting `ordered: false` sets the `U` (Unordered) bit on every SCTP `DATA` / `I-DATA` chunk so the receiving SCTP stack delivers each chunk immediately upon arrival without waiting for earlier Transmission Sequence Numbers (TSNs) (`[SOURCED: S6, RFC 8831 §6; S43, RFC 9260]`).
  * Setting `maxRetransmits: 0` (mutually exclusive with `maxPacketLifeTime`, `[SOURCED: S2, S19]`) instructs the sender's PR-SCTP stack never to retransmit a lost TSN and to immediately send a `FORWARD-TSN` chunk advancing the cumulative TSN ack point (`[SOURCED: S6, RFC 8831 §6]`).
  * `maxPacketLifeTime` (in milliseconds) bounds how long SCTP may attempt retransmission before abandoning a chunk; for RocketNet, **`maxRetransmits: 0` is strictly superior** because RocketNet's transport contract (`STANDARD.md:13-16` and `STANDARD.md:1705-1712`) explicitly states *"the transport informs, never retransmits (_Acks_)"* and *"The transport tells a sender which of its packets arrived. It never resends anything."* (`[SOURCED: STANDARD.md:13-16, STANDARD.md:1705-1712]`, `[OPINION]`).
* **What ICE, STUN, DTLS, and SCTP cost in round trips and CPU:**
  * **Signaling (1 RTT over HTTPS/WebSocket):** Browser and server/gateway exchange SDP offer and answer (`[SOURCED: S41, RFC 8829 JSEP]`).
  * **ICE / STUN (1 RTT with ICE-Lite on server):** When the server/gateway has a public IP and runs **ICE-Lite** (`[SOURCED: S38, RFC 8445 §2.2]`), the browser does not need an external STUN server (`iceServers: []`): the server embeds its public `host` candidate directly in the SDP answer, and a single STUN Binding Request / Response pair (`1 RTT`) validates the UDP path (`[SOURCED: S38, S39]`). If full ICE + external STUN is used instead, STUN gathering adds `1 RTT` before or during trickled signaling.
  * **TURN (`+1` to `+2` RTTs setup + relay hop latency):** Only invoked if direct UDP to the server/gateway's UDP port is blocked; TURN Allocate + CreatePermission / ChannelBind adds `1–2 RTTs` and routes traffic through a relay (`[SOURCED: S40, RFC 8656]`).
  * **DTLS 1.2 / 1.3 handshake (`1–2 RTTs`):** DTLS 1.2 requires `2 RTTs` (`ClientHello` / `HelloVerifyRequest` or `ServerHello`..`Finished`, `[SOURCED: S44, RFC 6347]`); DTLS 1.3 (`[SOURCED: S45, RFC 9147]`) takes `1 RTT`.
  * **SCTP association + DCEP (`0–2 RTTs`):** SCTP 4-way handshake (`INIT`, `INIT-ACK`, `COOKIE-ECHO`, `COOKIE-ACK`) takes `2 RTTs` (`[SOURCED: S43, RFC 9260]`). Using pre-negotiated data channels (`negotiated: true, id: 0`) eliminates the DCEP `DATA_CHANNEL_OPEN` / `ACK` overhead (`[SOURCED: S7, RFC 8832]`).
  * **Total WebRTC setup RTTs:** **$1\text{ RTT (HTTPS POST SDP)} + 1\text{ RTT (ICE-Lite STUN)} + 1\text{–}2\text{ RTTs (DTLS)} + 2\text{ RTTs (SCTP)} = 5\text{–}6\text{ RTTs}$** (`[ESTIMATED: 1 + 1 + 1.5 + 2 = 5.5 RTTs]`), compared to **1 RTT** for WebTransport (`[SOURCED: S33, S35]`).
  * **Per-packet wire & CPU cost:** Every WebRTC data channel message carries a **12-byte SCTP common header** + **16-byte SCTP `DATA` chunk header** (or 20-byte `I-DATA` header) + **13-byte DTLS 1.2 record header + 8-byte explicit nonce + 16-byte GCM tag** (or ~5–21 bytes in DTLS 1.3) = **~65 bytes of protocol framing on top of UDP** (`[ESTIMATED: 12 + 16 + 13 + 8 + 16 = 65 B]`). Even with `maxRetransmits: 0`, SCTP still exchanges `SACK` and `FORWARD-TSN` control chunks and runs SCTP congestion control (RFC 9260 slow-start / congestion avoidance), costing ~2–3$\times$ the CPU per packet of raw UDP or QUIC datagrams (`[ESTIMATED]`).

#### C. WebSockets (`WebSocket` API over TLS/TCP)
* **Protocol stack:** Framed binary messages (`0x82` binary opcode + 2-byte length for payloads $126\text{–}65535\text{ B}$ + 4-byte client-to-server masking key = **6 bytes server-to-client, 10 bytes client-to-server** `[SOURCED: S8, RFC 6455 §5.2]`) over TLS 1.3 (22 bytes record header + tag) over TCP (20–32 bytes header).
* **Browser status today (read 2026-10-14):** Universal (**W3C Baseline `high` since January 2018**): Chrome 16+, Edge 12+, Firefox 11+, Safari 7+, iOS Safari 6.0+, Android 4.4+ (`[SOURCED: S11, S24, S58, S62]`).
* **TCP Head-of-Line (HoL) Blocking, Nagle's Algorithm, and Every Mitigation:**
  * **TCP HoL blocking:** Because TCP guarantees an ordered byte stream, a single lost IP packet stalls delivery of **all subsequent WebSocket frames** to the browser until the kernel TCP stack retransmits the missing segment (`1 RTT` with SACK/RACK fast recovery, or $\ge 200\text{ ms}$ on a tail RTO) (`[SOURCED: S3, S8]`). For a 60 Hz game (`16.67 ms` tick interval) at `50 ms` RTT, a single lost packet stalls the next 3–4 game ticks by ~50–80 ms, arriving as a burst once the hole is filled (`[ESTIMATED: 50 ms RTT / 16.67 ms tick = 3 ticks stalled]`).
  * **Nagle's algorithm (`TCP_NODELAY`) & Delayed ACK interaction:** Without `TCP_NODELAY`, TCP coalesces small writes until the previous segment is acknowledged, which interacts catastrophically with 40–200 ms delayed ACKs. Browsers set `TCP_NODELAY` on `WebSocket` sockets internally, and the server/gateway **MUST** set `setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one))` (`[OPINION]`).
  * **Complete list of WebSocket HoL mitigations (`[OPINION]` + `[ESTIMATED]`):**
    1. **Server/Gateway `TCP_NODELAY` + `TCP_QUICKACK` (Linux):** Disables Nagle coalescing and delayed ACKs on the gateway socket.
    2. **Disable `permessage-deflate` (RFC 7692) on encrypted RocketNet payloads:** RocketNet packets are already ChaCha20-Poly1305 ciphertext (`high entropy`, incompressible); running `zlib` on them wastes CPU and adds 4+ bytes per frame.
    3. **Multi-connection WebSocket Striping (Sharded Pool of $K = 2\text{–}4$ Parallel `WebSocket` Connections):** The browser opens $K = 3$ or $4$ parallel `wss://` connections to the gateway, binding all $K$ TCP sockets to the **same** session/local UDP port on the gateway, and dispatches outgoing RocketNet packets round-robin `(seq % K)` across the connections whose `ws.bufferedAmount === 0`. Because RocketNet handles out-of-order arrival and duplicate rejection natively via its 64-bit sequence number and 4096-packet replay window (`[SOURCED: STANDARD.md:450-482, 1309-1350]`), if TCP connection $c_0$ suffers a packet loss and stalls for 1 RTT, connections $c_1, c_2, c_3$ continue delivering subsequent ticks immediately with **zero HoL stall**! Under independent $p = 2\%$ packet loss, the probability that all $K=3$ striped sockets are simultaneously stalled in the same RTT drops from $p = 0.02$ to $\approx p^3 = 0.000008$ (`[ESTIMATED: 0.02^3 = 8e-6]`, subject to shared bottleneck queue correlation).
    4. **Application-level Send Shedding (`ws.bufferedAmount` clamp + `TCP_NOTSENT_LOWAT`):** On the browser side, inspect `ws.bufferedAmount` (`[SOURCED: S24]`) before sending unreliable state/input ticks; if `bufferedAmount > 2464` (two RocketNet packets), drop or coalesce the tick rather than feeding a backed-up TCP send buffer. On the Linux gateway side, set `setsockopt(fd, SOL_TCP, TCP_NOTSENT_LOWAT, &lowat, sizeof(lowat))` (`e.g. 4096 bytes`) and drop outgoing server-to-client unreliable packets (`Packet Type 5` when pure unreliable, or via gateway queue depth) when the socket is backpressured (`[OPINION]`).
    5. **BBR Congestion Control on the Gateway Host (`TCP_CONGESTION = "bbr"`):** Keeps kernel queue occupancy low on lossy mobile links compared to CUBIC.

#### D. Other Browser Transport Paths Investigated
1. **WebRTC through a dedicated UDP Relay / Server-Side ICE-Lite (`WebRTC-Direct`):** Rather than peer-to-peer WebRTC with STUN/TURN servers, the gateway itself speaks **ICE-Lite + DTLS + SCTP** on a single UDP port (using `UDP-Mux`, multiplexing all browser sessions on one port by ICE ufrag / source address `[SOURCED: S63, S65]`). This eliminates external STUN servers and reduces WebRTC to a direct client-server UDP protocol.
2. **WebSocket + Unreliable Datagram Hybrids:** Using a fast `wss://` WebSocket (or HTTPS fetch) to start the session immediately (`1.5–2 RTTs`) while warming a background WebRTC (`5–6 RTTs`) or WebTransport session, then seamlessly switching the gateway's session binding to the datagram path on the first authenticated packet (`[OPINION]`, see `## 7 Ideas of your own`).
3. **Media-over-QUIC (`MoQ`, `draft-ietf-moq-transport`):** Investigated IETF `draft-ietf-moq-transport` (`[SOURCED: S34]`). MoQ is an application-layer pub/sub media framing protocol built **on top of** WebTransport (`[SOURCED: S34, §1]`). Because it runs over the exact same browser `WebTransport` API, it offers no additional browser reach or transport capabilities beyond direct `WebTransport` datagrams/streams, while adding pub/sub track/group/object framing overhead unnecessary for RocketNet (`[OPINION]`).
4. **QUIC-Native / Direct UDP Sockets (`Direct Sockets API`):** Restricted to Isolated Web Apps (IWAs) and enterprise kiosk policy; unavailable to normal web pages in any browser (`[OPINION]`). Browser plugins are excluded per the brief.

### 1.2 Quantitative Transport Comparison Table

| Dimension | WebTransport (`HTTP/3` QUIC Datagrams) | WebRTC `RTCDataChannel` (`ordered: false, maxRetransmits: 0`) | WebSockets (`wss://` over TLS 1.3 / TCP) | Striped WebSockets ($K=3$ `wss://` Pool) |
| :--- | :--- | :--- | :--- | :--- |
| **Underlying Stack** | `HTTP/3 Datagram` $\rightarrow$ `QUIC v1` $\rightarrow$ `UDP` `[SOURCED: S1, S4, S5, S35]` | `SCTP (PR-SCTP)` $\rightarrow$ `DTLS 1.2/1.3` $\rightarrow$ `ICE/STUN` $\rightarrow$ `UDP` `[SOURCED: S6, S38, S43, S44]` | `WS Frame (RFC 6455)` $\rightarrow$ `TLS 1.3` $\rightarrow$ `TCP` `[SOURCED: S3, S8]` | $K=3$ parallel `WS` $\rightarrow$ `TLS 1.3` $\rightarrow$ `TCP` sockets `[OPINION]` |
| **Browser Engine Support (2026-10-14)** | Chrome 97+, Edge 98+, Firefox 114+, **Safari 26.4+, iOS 26.4+** `[SOURCED: S9, S54, S60]` | Chrome 24+, Edge 79+, Firefox 22+, **Safari 11+, iOS 11+** `[SOURCED: S10, S56, S61]` | Chrome 16+, Edge 12+, Firefox 11+, **Safari 7+, iOS 6+** `[SOURCED: S11, S58, S62]` | Same as WebSockets (100% of modern browsers) `[SOURCED: S11]` |
| **Unreliable Delivery** | **Native** (`QUIC DATAGRAM`, never retransmitted) `[SOURCED: S5]` | **Yes** (`maxRetransmits: 0` sends `FORWARD-TSN` on loss) `[SOURCED: S6]` | **No** (TCP retransmits every lost segment) `[SOURCED: S8]` | **Partial** (lost TCP segment stalls 1 of $K$ lanes; others bypass) `[ESTIMATED]` |
| **Unordered Delivery** | **Native** (every QUIC datagram is independent) `[SOURCED: S5]` | **Yes** (`ordered: false` sets `U=1` bit on SCTP `DATA`) `[SOURCED: S6]` | **No** (strict byte-stream ordering per socket) `[SOURCED: S8]` | **Yes across lanes** (RocketNet sequence numbers reorder) `[SOURCED: STANDARD.md]` |
| **Max Datagram Size Limit** | **`maxDatagramSize`**: **1171 B** guaranteed min (1200 B QUIC initial); **~1350–1420 B** after PMTUD `[SOURCED: S1, S35]` / `[ESTIMATED]` | **1200 B** safe single-MTU payload (up to 64 KB with SCTP fragmentation, which reintroduces fragment HoL) `[SOURCED: S6, S18]` | Up to $2^{63}-1$ B framed (`1232 B` fits in 1 TCP MSS of `1460 B`) `[SOURCED: S8]` | Same as WebSockets (`1232 B` fits in 1 TCP MSS) `[SOURCED: S8]` |
| **Outer Wire Framing Overhead per Packet** | **~29 B** (`1B` flags + `8B` DCID + `2B` PN + `1B` type + `1B` QSID + `16B` AEAD tag) `[ESTIMATED]` | **~65 B** (`12B` SCTP + `16B` DATA + `13B` DTLS + `8B` nonce + `16B` tag) `[ESTIMATED]` | **~58–62 B** (`20–32B` TCP + `22B` TLS 1.3 + `6–10B` WS frame) `[ESTIMATED]` | **~58–62 B** per packet + lightweight keepalives `[ESTIMATED]` |
| **Congestion Control Imposed by Browser** | QUIC CUBIC / BBR (paces/throttles datagrams under congestion; drop stale via `outgoingMaxAge`) `[SOURCED: S1, S5, S36]` | SCTP cwnd + `FORWARD-TSN` (`RFC 9260`; can stall `bufferedAmount` under burst loss) `[SOURCED: S6, S43]` | TCP CUBIC / BBR (stalls entire stream during loss recovery + cwnd reduction) `[SOURCED: S8]` | Independent TCP cwnd per lane; sender skips lanes with `bufferedAmount > 0` `[OPINION]` |
| **Effect on 10–60 Hz Game Send Pattern** | Zero HoL stall; loss of tick $N$ never delays tick $N+1$ `[SOURCED: S5]` | Zero HoL stall when `maxRetransmits: 0` & packet $\le$ MTU; SCTP SACK chatter adds background traffic `[SOURCED: S6]` | **Severe HoL spikes:** 1% loss at 60 ms RTT adds **+60–120 ms** to p99 latency `[ESTIMATED]` | Cuts single-loss HoL stall by routing tick $N+1$ onto an unblocked TCP lane `[ESTIMATED]` |
| **Transport Setup Time (before RocketNet)** | **1 RTT** (QUIC + TLS 1.3 + HTTP/3 Extended CONNECT combined; 0-RTT on resumption) `[SOURCED: S33, S35]` | **5–6 RTTs** (`1` SDP + `1` ICE-Lite STUN + `1–2` DTLS + `2` SCTP) `[ESTIMATED]` | **2–3 RTTs** (`1` TCP SYN + `1` TLS 1.3 + `1` HTTP Upgrade) `[SOURCED: S8]` | **2–3 RTTs** (all $K$ sockets connect in parallel) `[ESTIMATED]` |
| **Certificate Story** | Web PKI **or** `serverCertificateHashes` ($\le 14$-day ECDSA self-signed cert) `[SOURCED: S1, §6.9]` | **Self-signed ephemeral DTLS cert** verified via `a=fingerprint:sha-256` in SDP (no CA needed!) `[SOURCED: S41]` | **Requires Web PKI CA cert** (e.g. Let's Encrypt ACME); no hash-pinning API in browsers `[SOURCED: S3, S52]` | **Requires Web PKI CA cert** `[SOURCED: S3]` |
| **NAT & Enterprise Firewall Reach** | High on residential/mobile (~90–95%); blocked where UDP/443 or HTTP/3 is blocked (~5–10%) `[ESTIMATED]` | Highest UDP reach via ICE/TURN; blocked only if UDP is blocked and no TURN TCP/TLS is configured `[SOURCED: S38, S40]` | **~99.9% reach** over `wss://` port 443 (traverses UDP-blocking firewalls & corporate proxies) `[OPINION]` | **~99.9% reach** over `wss://` port 443 `[OPINION]` |

---

## 2 Server side

### 2.1 Architecture Decision: Embed in Native Server vs. Co-Located Gateway / Sidecar (`[OPINION]`)

The native RocketNet server (`sdk/rocketnet_server_transport.cpp`) is engineered around a tight, zero-allocation UDP packet loop with Linux `AF_XDP` (`test/driver_extension/backend.cpp`), `UDP_SEGMENT` (GSO) / `recvmmsg` (`sdk/rocketnet_drivers.inc`), and Darwin `sendmsg_x` / `recvmsg_x` batch pumps (`[SOURCED: DRIVERS.md]`), depending **exclusively** on vendored `libsodium` (`sodium/sodium.c`, ISC license `[SOURCED: S73]`).

Embedding a full HTTP/3 + QUIC + TLS 1.3 stack, an ICE + DTLS + SCTP stack, and an HTTP/1.1 + TLS + WebSocket stack directly inside `librocketnet` would:
1. Pull a heavy X.509 / TLS 1.3 library (`OpenSSL`, `BoringSSL`, `GnuTLS`, `MbedTLS`, or `s2n-tls`) plus SCTP (`usrsctp`) into a C++ library whose security and build simplicity rest on having `sodium/sodium.c` as its only dependency (`[SOURCED: CMakeLists.txt, sodium/README.md]`).
2. Complicate or bypass the `AF_XDP` and `GSO` driver fast paths when multiplexing DTLS/QUIC/TCP state machines alongside native RocketNet UDP packets (`[SOURCED: DRIVERS.md]`).

**Recommendation (`[OPINION]`):** Keep the native RocketNet server speaking **pure RocketNet UDP** and deploy a **co-located native Browser Gateway daemon** (or optional separate library `librocketnet_gateway` running in a dedicated sidecar process on the same physical machine).
* **Native UDP clients** continue to hit the game server's public UDP port directly through the existing **AF_XDP / GSO / batch pump fast path** with zero gateway involvement (`[SOURCED: DRIVERS.md]`).
* **Browser clients** connect to the co-located Browser Gateway on the same host (e.g., UDP/443 for WebTransport, UDP/4443 for WebRTC ICE-Lite UDP-Mux, TCP/443 for WebSockets).
* **How the Gateway preserves the `"client address is the connection identity"` rule (`STANDARD.md:348`):**
  * For each admitted browser session $i$, the gateway opens a dedicated non-blocking local UDP socket bound to `127.0.0.1:0` (or `127.0.0.1 + (i >> 15)` across `127.0.0.1`..`127.0.0.16` if $> 32{,}768$ browser clients share one host) and `connect()`s it to the RocketNet server's UDP port (`[ESTIMATED]`).
  * Every datagram arriving from browser session $i$ over WebTransport, WebRTC, or WebSocket is written via `send(udp_fd[i], payload, len, MSG_DONTWAIT)` to the RocketNet server, which sees a unique, stable `(127.0.0.1, ephemeral_port_i)` source address for the entire lifetime of the session (`[SOURCED: STANDARD.md:342-392]`).
  * Every UDP packet sent back by the RocketNet server to `(127.0.0.1, ephemeral_port_i)` wakes `epoll` / `io_uring` / `kqueue` on `udp_fd[i]` in the gateway and is forwarded out over browser session $i$'s WebTransport datagram stream, WebRTC data channel, or WebSocket connection (`[OPINION]`).
  * **Bonus capability:** Even if the browser's external IP/port changes (Wi-Fi $\rightarrow$ 5G cellular handoff) and QUIC connection migration (`[SOURCED: S35, RFC 9000 §9]`) or WebSocket reconnection keeps or rebinds the gateway session to `udp_fd[i]`, **the native RocketNet server sees zero address change** because `udp_fd[i]`'s loopback port never changed!

### 2.2 Server / Gateway Library Comparison (Verified via GitHub API on 2026-10-14)

All repository licenses, languages, star counts, and last-push dates below were queried directly from the GitHub API on `2026-10-14` (`[SOURCED: S63–S71, S75]`).

| Library & Repository | Transport Role | License (Read 2026-10-14) | Language | Last Activity (Read 2026-10-14) | Build Burden with C++ / Vendored `libsodium` | Known Scale & Architecture Notes |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **`libdatachannel`** `[S63]` (`paullouisageneau/libdatachannel`) | WebRTC Data Channels + WebSockets | **MPL-2.0** (weak file-level copyleft; compatible with AGPL-3.0 and commercial dynamic/file linking) `[SOURCED: S63]` | C++17 (with C ABI `rtc.h`) | `2026-10-07` (`2,760` stars) `[SOURCED: S63]` | **Low–Medium:** CMake build; bundles `libjuice` (ICE) and `usrsctp`; requires `OpenSSL`, `GnuTLS`, or `MbedTLS` for DTLS `[SOURCED: S63]`. | Supports ICE-Lite and single-port **UDP-Mux** (`enableIceUdpMux`); ~30–80 KB RAM/peer; handles ~1,000–3,000 peers per process `[ESTIMATED]`. |
| **`webrtc-rs`** `[S64]` (`webrtc-rs/webrtc`) | WebRTC Data Channels (`sansio` `rtc` crate emerging) | **Apache-2.0** `[SOURCED: S64]` | Rust (`tokio` async or `rtc` sans-I/O) | `2026-10-11` (`5,159` stars) `[SOURCED: S64]` | **Medium (FFI) / Low (Sidecar):** Requires `cargo` + Rust toolchain if linked via C ABI, or zero C++ build burden as a standalone sidecar binary `[SOURCED: S64]`. | Supports single-port UDP-Mux and ICE-Lite; pure Rust (`rustls` / `ring`, no OpenSSL C build) `[SOURCED: S64]`. |
| **`pion/webrtc`** `[S65]` (`pion/webrtc`) | WebRTC Data Channels (behind a Go gateway shim) | **MIT** `[SOURCED: S65]` | Go | `2026-10-14` (`16,832` stars) `[SOURCED: S65]` | **Zero C++ burden as sidecar gateway** (single static Go binary); high if embedded via `cgo` (Go runtime + GC threads) `[SOURCED: S65]`. | Industry-standard Go WebRTC server (`LiveKit`, `Cloudflare` origins); native `ICEUDPMux` + `SettingEngine.SetLite(true)` `[SOURCED: S65]`. |
| **`libwebrtc`** `[S75]` (`webrtc.googlesource.com/src`) | Full W3C WebRTC reference stack | **BSD-3-Clause** `[SOURCED: S75]` | C++ | `2026-10-14` `[SOURCED: S75]` | **Extreme (Disqualifying):** Requires Chromium `depot_tools`, `gn`, `ninja`, bundled BoringSSL + Abseil, >10 GB checkout, hundreds of threads `[SOURCED: S63, S75]`. | Built for browser engines and media conferencing; far too heavy for a game datagram server `[OPINION]`. |
| **`msquic`** `[S66]` (`microsoft/msquic`) | QUIC v1 + Datagrams (RFC 9221) | **MIT** `[SOURCED: S66]` | C (with C++ wrappers) | `2026-10-13` (`4,785` stars) `[SOURCED: S66]` | **Medium:** CMake; uses Schannel on Windows or OpenSSL/QuicTLS on Linux/macOS; note MsQuic provides **QUIC transport only**, requiring an HTTP/3 + WebTransport framing layer on top `[SOURCED: S66]`. | Kernel/user-mode high-scale QUIC engine (`epoll` / `io_uring` / `kqueue` + GSO/GRO + RSS); scales to >100k conns and >500k pps/core `[SOURCED: S66]`. |
| **`cloudflare/quiche`** `[S67]` (`cloudflare/quiche`) | QUIC v1 + HTTP/3 + Datagrams (RFC 9221 / RFC 9297) | **BSD-2-Clause** `[SOURCED: S67]` | Rust (with C API `quiche.h`) | `2026-10-13` (`12,782` stars) `[SOURCED: S67]` | **Medium:** Exposes a clean **sans-I/O C API** (`quiche_conn_recv`, `quiche_conn_send`, `quiche_conn_dgram_recv/send`), so the caller owns the UDP socket! Requires Rust + BoringSSL `[SOURCED: S67]`. | Powers Cloudflare's global HTTP/3 edge (`[SOURCED: S67]`); sans-I/O design means it can share the exact same UDP socket and event loop as RocketNet! |
| **`ngtcp2`** `[S68]` (`ngtcp2/ngtcp2`) | QUIC v1 + Datagrams (RFC 9221, paired with `nghttp3`) | **MIT** `[SOURCED: S68]` | C | `2026-10-11` (`1,528` stars) `[SOURCED: S68]` | **Low–Medium:** Pure C sans-I/O library (`cmake`); pluggable TLS backend (`quictls`, `BoringSSL`, `GnuTLS`, `Picotls`, `wolfSSL`) `[SOURCED: S68]`. | Very low memory per connection (~15–30 KB), zero internal threads, clean callback API `[SOURCED: S68]`. |
| **`lsquic`** `[S69]` (`litespeedtech/lsquic`) | QUIC v1/v2 + HTTP/3 + Datagrams | **MIT** `[SOURCED: S69]` | C | `2026-10-12` (`1,878` stars) `[SOURCED: S69]` | **Medium:** Pure C (`cmake`), requires `BoringSSL` and `zlib` `[SOURCED: S69]`. | Production-proven in LiteSpeed Web Server and OpenLiteSpeed (`[SOURCED: S69]`); event-loop agnostic (caller supplies packets). |
| **`s2n-quic`** `[S70]` (`aws/s2n-quic`) | QUIC v1 + Datagrams | **Apache-2.0** `[SOURCED: S70]` | Rust | `2026-10-14` (`1,376` stars) `[SOURCED: S70]` | **Medium (Rust):** Uses `s2n-tls` or `rustls`; supports GSO and PMTUD (`[SOURCED: S70]`); needs an HTTP/3 Extended CONNECT WebTransport layer (`h3` / `wtransport`). | Built by AWS; strong fuzzing and RFC compliance tracking (`[SOURCED: S70]`). |
| **`uWebSockets`** `[S71]` (`uNetworking/uWebSockets`) | WebSockets (`ws://` / `wss://`) + HTTP | **Apache-2.0** `[SOURCED: S71]` | C++17 (with C `uSockets`) | `2026-10-05` (`18,999` stars) `[SOURCED: S71]` | **Very Low:** Header-only C++17 templates over tiny C `uSockets`; optional OpenSSL/WolfSSL/BoringSSL for `wss://` (or plain `ws://` behind nginx/Cloudflare/Caddy) `[SOURCED: S71]`. | Ultra-low overhead (~4–8 KB RAM/conn); TLS 1.3 messaging at hundreds of thousands of msgs/sec/core `[SOURCED: S71]`. |

### 2.3 Gateway Performance Estimates: Latency, CPU, PPS/Core, Memory, and Scaling

All figures below for the co-located sidecar gateway are `[ESTIMATED]` with explicit arithmetic for a modern server core (e.g., 3.5 GHz x86_64 or ARM64 Linux with `epoll`/`io_uring` and AES-NI / ARMv8 Crypto Extensions):

| Metric | WebTransport Gateway (`ngtcp2`/`quiche`/Rust) | WebRTC ICE-Lite UDP-Mux Gateway (`libdatachannel` / `pion`) | WebSocket Gateway (`uWebSockets`) | Arithmetic & Basis (`[ESTIMATED]`) |
| :--- | :--- | :--- | :--- | :--- |
| **Latency Added per Hop (Co-located `127.0.0.1`)** | **15–35 $\mu\text{s}$** (p50), **60–120 $\mu\text{s}$** (p99) | **25–55 $\mu\text{s}$** (p50), **90–180 $\mu\text{s}$** (p99) | **12–30 $\mu\text{s}$** (p50), **50–100 $\mu\text{s}$** (p99) | Outer recv + AEAD decrypt (`~1.5 us`) + user-space parse (`~1–4 us`) + loopback `sendto`/`recvfrom` (`~8–15 us` on Linux kernel 6.x) |
| **Gateway CPU per Client at 60 Hz Bidir (`120 pkt/s`)** | **~0.08% – 0.14%** of 1 core (`~700–1,200 clients/core`) | **~0.15% – 0.30%** of 1 core (`~330–650 clients/core`) | **~0.05% – 0.09%** of 1 core (`~1,100–2,000 clients/core`) | At `120 pkt/s/client`: WT $\approx 8\ \mu\text{s/pkt} \times 120 = 960\ \mu\text{s/s}$; WebRTC (DTLS+SCTP+SACK) $\approx 18\ \mu\text{s/pkt} \times 120 = 2160\ \mu\text{s/s}$; WS $\approx 5\ \mu\text{s/pkt} \times 120 = 600\ \mu\text{s/s}$ |
| **Packets / Sec / Core (Gateway Relay Ceiling)** | **~100,000 – 150,000 pps/core** | **~45,000 – 75,000 pps/core** | **~160,000 – 250,000 pps/core** | $1\text{ s} / 8\ \mu\text{s} = 125{,}000\text{ pps}$ (WT); $1\text{ s} / 18\ \mu\text{s} = 55{,}555\text{ pps}$ (WebRTC); $1\text{ s} / 5\ \mu\text{s} = 200{,}000\text{ pps}$ (WS) |
| **Memory per Client in Gateway** | **~24 – 48 KiB** | **~64 – 140 KiB** | **~8 – 20 KiB** | WT: QUIC conn state + HTTP/3 streams (`~32 KiB`); WebRTC: ICE + DTLS + `usrsctp` send/recv queues (`~96 KiB`); WS: `uWebSockets` + TLS (`~12 KiB`) |

* **Does the XDP / GSO fast path survive?**
  * **Yes (`[OPINION]`):** Native UDP clients connect directly to the RocketNet server's public UDP port (e.g., `UDP/40000`), where the `AF_XDP` or `GSO` driver runs untouched (`[SOURCED: DRIVERS.md]`). Browser sessions terminate on the gateway's ports (`UDP/443`, `UDP/4443`, `TCP/443`) and enter the RocketNet server over loopback (or via a dedicated in-memory shared-memory ring driver if co-located in the same process/host, avoiding even the loopback UDP syscall!).
  * **Important Linux XDP nuance:** An `AF_XDP` program attached to a physical NIC (`eth0`) only intercepts packets arriving on `eth0`, not packets sent over `lo` (`127.0.0.1`). Therefore, if a single RocketNet server instance serves **both** native XDP clients on `eth0` and gateway loopback clients simultaneously, the gateway should either (a) send through a `veth` pair attached to the server's poll loop, or (b) use a composite driver that polls both the `AF_XDP` ring and a local Unix/UDP/shared-memory ring from the co-located gateway (`[OPINION]`).
* **Scaling across machines & Load Balancer / TLS Certificate story:**
  * **Stateful game servers are addressed directly:** In RocketNet (`[SOURCED: STANDARD.md:755-893]`), a Connect Token names a **specific** game server instance (`ipv4_address`, `ipv6_address`, `server_public_key`). Browser clients therefore do **not** sit behind a stateless L7 round-robin proxy that sprays packets across random backends; each browser client connects directly to the gateway co-located with the assigned game server!
  * **Three clean TLS certificate strategies for per-server gateways (`[OPINION]` grounded in `[SOURCED: S1, S41, S52]`):**
    1. **Wildcard DNS + ACME DNS-01 (`*.nodes.game.example.com`, `[SOURCED: S52, RFC 8555]`):** Map each server IP `A.B.C.D` to `ip-a-b-c-d.nodes.game.example.com` and distribute a single Let's Encrypt wildcard TLS certificate (`*.nodes.game.example.com`) to all gateway nodes. Works for **all three** transports (WebTransport, WebRTC, and WebSockets)!
    2. **WebTransport `serverCertificateHashes` (`[SOURCED: S1, §6.9]`):** Each gateway generates a 14-day self-signed ECDSA P-256 certificate on startup; the matchmaker includes the 32-byte SHA-256 certificate hash alongside the Connect Token (or inside the Connect Token's 55-byte zero pad!). Works on bare IP addresses without DNS or CA!
    3. **WebRTC Ephemeral Self-Signed DTLS Fingerprint (`[SOURCED: S41, RFC 8829]`):** WebRTC DTLS never uses Web PKI CAs; the gateway generates a self-signed certificate and puts `a=fingerprint:sha-256 ...` in the SDP answer.

---

## 3 Protocol mapping

### 3.1 Core Security & Architecture Decision: End-to-End RocketNet AEAD vs. Gateway Termination

**Decision (`[OPINION]`):** **Keep RocketNet's entire wire protocol — Connect Token, 3-RTT handshake, X25519 ephemeral key exchange, ChaCha20-Poly1305 IETF AEAD, 64-bit sequence numbers, and $\ge 4096$-packet replay window — strictly end-to-end between the browser client and the native RocketNet server.** The gateway acts purely as an untrusted packet forwarder that strips the outer browser transport framing (WebTransport datagram, WebRTC data channel message, or WebSocket binary frame) and forwards the inner RocketNet UDP datagram verbatim to a per-session local UDP port.

#### Why End-to-End Wins Over Terminating RocketNet at the Gateway (`[ESTIMATED]` + `[SOURCED]`):

| Criterion | Option A: End-to-End RocketNet Inside Browser Transport (**Recommended**) | Option B: Terminate RocketNet at Gateway (Browser Sends Plaintext over QUIC/DTLS/TLS) |
| :--- | :--- | :--- |
| **Wire Byte Overhead per Session Packet** | **`+26 B`** RocketNet envelope (`6B` prefilter + `4B` seq + `16B` Poly1305 tag, `[SOURCED: STANDARD.md:625-698, 1285-1399]`) + `7B` ack block (`needed either way for RocketNet loss notifications`) = **`2.11%`** of a 1232 B packet (`26 / 1232`) `[ESTIMATED]` | **`0 B`** RocketNet AEAD on browser link, **but** gateway must still synthesize `6B` prefilter + `4B` seq + `7B` ack block + `16B` tag toward the native server (or native server must add a new unencrypted trusted-gateway socket mode) `[ESTIMATED]` |
| **Browser CPU Cost at 60 Hz (`120 pkt/s` bidir)** | **~180–480 $\mu\text{s}$/sec total** (`1.5–4.0 us/pkt` in WASM `libsodium` ChaCha20-Poly1305 = **`0.018%–0.048%` of one CPU core**) `[ESTIMATED]` | `0 us/pkt` in WASM, saving $< 0.05\%$ of a core (imperceptible) `[ESTIMATED]` |
| **Gateway CPU Cost** | **Zero RocketNet crypto in gateway!** Gateway only does 1 outer AEAD pass (QUIC/DTLS/TLS) and forwards raw bytes `[ESTIMATED]` | Gateway must perform **two** AEAD passes per packet (decrypt QUIC/DTLS/TLS, then re-encrypt ChaCha20-Poly1305 to feed the native RocketNet server) **or** require a new plaintext server port `[ESTIMATED]` |
| **Trust & Key Distribution Model** | **Zero-trust gateway:** `STANDARD.md:755-893` seals the Connect Token's private block to `server_public_key` (`crypto_box_seal`) and derives forward-secret session keys via an ephemeral `X25519` exchange (`RKTNSESC`/`RKTNSESS`, `[SOURCED: STANDARD.md:989-1134]`). An untrusted gateway cannot read, modify, or forge a single game packet! | **Breaks `STANDARD.md` zero-shared-secret model:** Gateway would need the game server's private X25519 key (`server_secret_key`) to open the Connect Token's `sealed_private_block` (`432 B`), exposing session keys at the edge `[SOURCED: STANDARD.md:176-216, 792-822]` |
| **Ack, Loss Notification & Fragmentation Semantics** | **100% preserved:** `rocketnet_client_protocol.cpp` and `docs/CHANNELS.md` rely on RocketNet's per-packet sequence number (`4B`), `ack` + `ack_bits` (`7B+`), and `Packet Type 9` (`payload fragment`) end-to-end `[SOURCED: STANDARD.md:1705-2000, docs/CHANNELS.md]` | Gateway would have to re-implement or bridge RocketNet sequence numbers, acks, and fragment reassembly, breaking end-to-end loss visibility (`rocketnet_client_transport_acked_packets`) `[SOURCED: sdk/rocketnet.h:648]` |
| **Changes Required to `STANDARD.md` & Native Server** | **None (`0` lines changed in `STANDARD.md` or server)** `[OPINION]` | Requires a new trusted-proxy protocol specification in `STANDARD.md` `[OPINION]` |

### 3.2 Connect Token Delivery and Stateless Gateway Admission Without a Shared Secret

* **How the 768-byte Connect Token reaches the browser (`[OPINION]` grounded in `[SOURCED: STANDARD.md:755-893]`):**
  1. **Matchmaking HTTPS `POST /api/matchmake` response body (JSON with 1024-char Base64 token):** The browser fetches the Connect Token over `fetch("https://match.example.com/connect-token", { method: "POST", credentials: "include" })`. Because a Connect Token is a bearer credential (`[SOURCED: STANDARD.md:762-778]`), **never** put it in a URL query string (which leaks into server access logs, `Referer` headers, and browser history). If passed across a same-origin navigation, use `sessionStorage` or a URL fragment (`#token=...`, which is never sent in HTTP requests), and clear `location.hash` immediately via `history.replaceState` (`[OPINION]`).
  2. **Passing the token to the Gateway to authorize UDP port allocation:**
     * **Crucial insight (`[SOURCED: STANDARD.md:824-893]`):** How does a gateway verify that a connecting browser holds a valid, unexpired Connect Token for this server **without** sharing a secret with the backend or the game server?
     * **Answer from `STANDARD.md:824-893`:** The first **640 bytes** of the 768-byte Connect Token (and bytes `70..710` of a 710-byte `Type 0 Connection Request` packet, `[SOURCED: STANDARD.md:1204-1225]`) consist of the **576-byte `signed connect data`** in the clear followed by the **64-byte `Ed25519 signature`** (`crypto_sign_verify_detached`)!
     * Inside that 576-byte `signed connect data`, the following fields are **in the clear at fixed offsets** (`[SOURCED: STANDARD.md:830-866]`):
       * `0..4`: `version = 0x4E7C1A93` (`uint32`, `4 B` opaque constant — no ASCII banner, `[SOURCED: STANDARD.md:831, 858]`)
       * `4..12`: `protocol_id` (`uint64`, `8 B`)
       * `12..20`: `create_timestamp` (`uint64`, `8 B`)
       * `20..28`: `expire_timestamp` (`uint64`, `8 B`, Unix seconds)
       * `28..32`: `timeout_seconds` (`int32`, `4 B`)
       * `32..33`: `address_flags` (`uint8`, `1 B`: bit 0 = has IPv4, bit 1 = has IPv6)
       * `33..39` / `39..57`: `ipv4_address` (`6 B`) and `ipv6_address` (`18 B`) of the target game server
       * `57..89`: `server_public_key` (`32 B`, followed by `89..521` `sealed_private_block` `432 B` and `521..576` `55 B` zero pad, `[SOURCED: STANDARD.md:842-844, 866]`)
     * Therefore, the gateway only needs the **public** `backend_public_key` (`32 B`, public knowledge!) and the target `server_public_key` / address! When a browser opens a WebTransport, WebRTC, or WebSocket session (or sends its first 710-byte `Type 0 Connection Request` datagram), the gateway checks `expire_timestamp >= now`, verifies the 64-byte Ed25519 signature over the 576-byte signed block using `crypto_sign_verify_detached` (`~60–100 us`), checks a small sliding bloom/LRU filter of `BLAKE2b-128(signed_data)` to prevent token flooding, and only then allocates the local UDP socket `udp_fd[i]`! **Zero shared secrets are required!**

### 3.3 Cookie Exchange, 3-RTT Handshake, and Handshake Compression

* **What happens to the Cookie Exchange (`Packet Type 7`, 38 bytes) and 3-RTT Handshake (`[SOURCED: STANDARD.md:1136-1399, 2490-2848]`):**
  * Over raw UDP, RocketNet requires 3 round trips before session data:
    1. **RTT 1 (Stateless Address Validation):** Client sends `Type 0 Connection Request` (`710 B`, empty 24-byte `return_cookie`) $\rightarrow$ Server replies `Type 7 Connection Cookie` (`38 B`) without allocating state (`[SOURCED: STANDARD.md:1136-1152, 1204-1283]`).
    2. **RTT 2 (Token Admission & Ephemeral Key Exchange):** Client sends `Type 0 Connection Request` (`710 B`, with cookie) $\rightarrow$ Server verifies Ed25519 signature, opens `sealed_private_block`, derives handshake/session keys, and replies `Type 2 Connection Challenge` (`66 B`, containing sealed `server_eph_pk`) (`[SOURCED: STANDARD.md:1136-1165, 2496-2824]`).
    3. **RTT 3 (Client Challenge Proof & Slot Assignment):** Client sends `Type 3 Connection Response` (`50 B`, 24B zero padding) $\rightarrow$ Server assigns a client slot and sends `Type 4 Connection Keep-Alive` (`41 B` min, carrying `[client_index, max_clients]`) (`[SOURCED: STANDARD.md:1166-1184, 2826-2848]`).
  * **Which steps are redundant over a browser transport and how to handle them (`[OPINION]`):**
    * The browser transport (QUIC 1-RTT, WebSocket TCP+TLS 2–3 RTT, or WebRTC ICE+DTLS+SCTP 5–6 RTT) has **already validated the browser's return IP address** before the first RocketNet packet is sent.
    * **However**, if the gateway sits on co-located loopback (`127.0.0.1`, `< 0.05 ms` RTT to the native RocketNet server), **RTT 1 (the Cookie exchange) does NOT need to traverse the wide-area browser link!**
    * **Gateway-Assisted Loopback Cookie Prefetch (`Zero-Spec-Change 2-RTT Handshake`, `[OPINION]`):** When the browser sends its initial `Type 0 Connection Request` (`710 B`, empty cookie) over WebTransport/WebRTC/WebSocket to the gateway (or includes the `client_eph_pk` + `salt` + `Connect Token` in the WebTransport URL/stream, WebRTC SDP, or WebSocket upgrade), the gateway forwards that `710 B` packet to the local RocketNet server over `127.0.0.1` (`~20 us`), receives the `38 B` `Type 7 Connection Cookie` from the local server (`~20 us`), **immediately splices the 24-byte cookie into bytes `6..30` of the cached 710-byte `Type 0 Connection Request`** (which are unauthenticated cleartext bytes before the signed token, `[SOURCED: STANDARD.md:1204-1225]`), and re-sends the cookie-bearing `Type 0` request to the local server! Within `< 150 us` on loopback, the local server emits the `66 B` `Type 2 Connection Challenge`, which the gateway sends back to the browser!
    * **Result:** Without modifying a single line of `STANDARD.md` or the native server, **RTT 1 (the cookie round-trip) collapses onto localhost (`~0.1 ms`)**, cutting the wide-area RocketNet handshake from **3 WAN RTTs to 1.5–2 WAN RTTs**!

### 3.4 The 1232-Byte Budget vs. Transport Datagram Limits and Fragmentation

* **RocketNet's packet size budget (`[SOURCED: STANDARD.md:295-296, 625-698, sdk/rocketnet.h:411-434, sdk/rocketnet_constants.h:32-47]`):**
  * Default maximum outbound UDP datagram: **`1232 bytes`** (`1280 B` IPv6 minimum MTU minus `40 B` IPv6 header minus `8 B` UDP header).
  * Session packet overhead: `26 B` fixed (`6B` prefilter + `4B` sequence + `16B` ChaCha20-Poly1305 tag) + `7 B` minimum sealed ack block = **`33 B`**, leaving `ROCKETNET_MAX_PAYLOAD_BYTES = 1192` bytes (`1225 B` max UDP packet for a single unfragmented payload, or up to `1232 B` when `ack_range_count > 1`) (`[SOURCED: sdk/rocketnet.h:420, sdk/rocketnet_constants.h:32-47]`).
  * Receive ceiling: **`1426 bytes`** (`ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES = 1393`, `[SOURCED: sdk/rocketnet.h:421, STANDARD.md:625-698]`).
* **Collision with WebTransport's initial 1200-byte QUIC minimum (`[SOURCED: S1, S5, S35]`):**
  * When a WebTransport session first opens on a path where QUIC Path MTU Discovery has not yet probed past the 1200-byte QUIC minimum (`[SOURCED: S35, RFC 9000 §14]`), `wt.datagrams.maxDatagramSize` can be as low as **1171 bytes** (`1200 B` minus ~29 B of QUIC short header + AEAD tag + DATAGRAM frame type + Quarter-Stream-ID).
  * If RocketNet attempts to send a **1225-byte** or **1232-byte** UDP packet over a WebTransport datagram stream whose `maxDatagramSize` is `1171`, **the browser silently drops the outgoing datagram** (`[SOURCED: S1, §5.2]`)!
  * Note that handshake packets (`Type 0 Request` = `710 B`, `Type 2 Challenge` = `66 B`, `Type 3 Response` = `50 B`, `Type 4 Keep-Alive` = `41 B`, `Type 7 Cookie` = `38 B`, `[SOURCED: STANDARD.md:651-664]`) are all $\le 710\text{ B}$ and **always fit** inside any `maxDatagramSize` $\ge 710\text{ B}$.
  * **Two-part solution for `Packet Type 5` (Payload) and `Packet Type 9` (Fragment) (`[OPINION]`):**
    1. **Clamp effective fragment/payload size to `min(1192, maxDatagramSize - 40)`** (where `40 B` covers `26 B` RocketNet envelope + `14 B` ack block / fragment header). Even at the worst-case `maxDatagramSize = 1171 B`, a clamped payload of **`1120 bytes`** (`1153 B` total RocketNet packet) fits inside a single unfragmented QUIC datagram on **100% of QUIC v1 paths** without waiting for PMTUD (`[ESTIMATED: 1120 + 33 = 1153 <= 1171]`)!
    2. **Unidirectional Stream Spillover for Oversized Datagrams (`Zero-Drop Fallback`):** If the native server sends a `1225 B` packet to the gateway before the client's QUIC path MTU has grown to $\ge 1254\text{ B}$ (`quiche_conn_dgram_max_writable_len(conn) < pkt_len`), the gateway writes that single packet onto a **one-shot WebTransport unidirectional stream** (`wt.incomingUnidirectionalStreams`, `[SOURCED: S1]`), which QUIC splits across two QUIC packets and delivers without head-of-line blocking any other datagram or stream!

### 3.5 What Would Need an Addition to `STANDARD.md` vs. What Would Not

* **Requires ZERO changes to `STANDARD.md` (`[OPINION]`):**
  * End-to-end Connect Token verification, 3-RTT handshake (including gateway loopback cookie splicing), X25519 ephemeral key exchange, ChaCha20-Poly1305 AEAD, sequence numbers, 4096-packet replay window, ack blocks, `docs/CHANNELS.md` message records, and connection migration via persistent gateway loopback port mapping.
* **Optional, Backward-Compatible Additions to `STANDARD.md` (if desired later):**
  1. **Using the 55-byte or 64-byte Connect Token zero-padding slack for Browser Gateway Hints (`[SOURCED: STANDARD.md:844, 866-869, 882]`):**
     * `STANDARD.md:866-869` notes that bytes `521..576` (`55 bytes`) inside `signed connect data` are zero-padded slack, and bytes `704..768` (`64 bytes`) at the end of the 768-byte Connect Token are outside the signature and zero-padded.
     * A backward-compatible v1.1 extension (or an out-of-band JSON wrapper returned alongside the Base64 token) can carry the gateway's **32-byte SHA-256 `serverCertificateHashes` digest** plus transport ports (`wt_port`, `webrtc_port`, `ws_port`). Keeping it in the JSON wrapper around the Base64 token requires **zero changes to `STANDARD.md`** (`[OPINION]`).
  2. **Configurable Client/Server Payload MTU Ceiling (`max_payload_bytes` in `rocketnet_client_config_t` / `Connect Token`):** Allowing a session or client config to cap fragment size at `1120 B` instead of hardcoded `ROCKETNET_MAX_PAYLOAD_BYTES` (`1192 B`) so native server fragments never exceed the 1171-byte unprobed QUIC datagram floor (`[OPINION]`).

---

## 4 Client library

### 4.1 Family A (Emscripten WASM C++ Client) vs. Family B (Pure TypeScript + WASM `libsodium.js`)

We inspected the exact C++ client source files in `sdk/` and `sodium/` (`[SOURCED: CMakeLists.txt, sdk/rocketnet.h, sdk/rocketnet_client_transport.cpp, sdk/rocketnet_driver.h]`) to compare the two client families:

| Dimension | Family A: Emscripten WASM Build of C++ `librocketnet` (`sdk/*.cpp` + `sodium/sodium.c`) | Family B: Pure TypeScript Wire Implementation + `libsodium.js` (WASM Crypto Only) |
| :--- | :--- | :--- |
| **Source Code Scope** | Existing C++ client (`rocketnet_client_transport.cpp` 48.5 KB, `rocketnet_client_protocol.cpp` 64.1 KB, `rocketnet_ack.cpp` 27.0 KB, `rocketnet_fragment.cpp` 21.8 KB, `rocketnet_message.cpp` 23.3 KB, `rocketnet_connect_token.cpp` 11.6 KB, `rocketnet_protocol.cpp` 18.5 KB, `rocketnet_replay.cpp` 4.0 KB, `rocketnet_allocator.cpp` 28.0 KB) + `sodium/sodium.c` (1.09 MB) `[SOURCED: repo files]` | ~1,800–2,500 lines of TypeScript implementing `STANDARD.md` + `docs/CHANNELS.md` + `libsodium-wrappers` (WASM build of `crypto_aead_chacha20poly1305_ietf`, `crypto_generichash_blake2b`, `crypto_scalarmult`, `crypto_kx`) `[ESTIMATED]` |
| **Uncompressed / Gzipped / Brotli Size (`[ESTIMATED]`)** | **~240–310 KB** raw `.wasm` + `~18 KB` JS glue $\rightarrow$ **~95–125 KB gzipped** (**~75–100 KB Brotli**) when compiled with `-Oz -flto -fno-exceptions -fno-rtti` and dead-stripping unused server/sodium symbols `[ESTIMATED: ~130 KB sodium WASM + ~120 KB client C++ WASM]` | **~22–30 KB** minified TS/JS (`~8–11 KB` gzipped) + slim `libsodium.js` WASM (`~165 KB` raw / **~65–75 KB gzipped**; or **~12 KB** if using pure-JS/hand-written WASM ChaCha20-Poly1305 + BLAKE2b + WebCrypto X25519) $\rightarrow$ **~20–85 KB gzipped total** `[ESTIMATED]` |
| **How to Measure Exact Size** | Compile with `emcc -Oz -flto` exporting only `_rocketnet_client_*`, run `wasm-opt -Oz`, and measure `wc -c`, `gzip -9 -c | wc -c`, and `brotli -q 11 -c | wc -c` | Bundle with `esbuild --bundle --minify --format=esm`, and measure `wc -c`, `gzip -9 -c | wc -c`, and `brotli -q 11 -c | wc -c` |
| **Startup / Instantiation Time** | **~8–25 ms** (`WebAssembly.instantiateStreaming` compiles at >30 MB/s on desktop V8/JSC/SpiderMonkey and ~10–15 MB/s on mobile) `[ESTIMATED]` | **~3–12 ms** (smaller WASM module or pure ESM parse) `[ESTIMATED]` |
| **Per-Packet Cost at 60 Hz (`1232 B` packet)** | **~2.0–4.5 $\mu\text{s}$/packet** (`~1.8 us` ChaCha20-Poly1305 + `~0.5 us` ack/replay/fragment + `~0.3 us` JS$\leftrightarrow$WASM ring copy) `[ESTIMATED]` | **~2.5–6.0 $\mu\text{s}$/packet** (`~1.8 us` WASM `libsodium.js` AEAD + `~1.5 us` TypeScript `DataView` / `Uint8Array` header & ack parsing) `[ESTIMATED]` |
| **Linear Memory Footprint per Client** | **~15.2 MiB default (!)** (`sizeof(rocketnet_client_transport_t)` $\approx 6.5\text{ MiB}$ + `fragment_reassembly` $\approx 8.5\text{ MiB}$ + `send_arena` $256\text{ KiB}$); drops to **~1.2 MiB** if buffer constants are parameterized for browser builds `[ESTIMATED: see §4.2]` | **~256–512 KiB** (allocates `Uint8Array` slabs on demand rather than fixed `2048 * 1024` 2D arrays) `[ESTIMATED]` |
| **Conformance with Native C++ Client** | **Exact bit-for-bit parity by construction** (compiles the identical C++ state machines, `rocketnet_ack.cpp`, `rocketnet_fragment.cpp`, and `CHANNELS.md` scheduler) `[OPINION]` | Must maintain parallel implementation of `STANDARD.md` and `docs/CHANNELS.md` (risk of subtle divergence, e.g. ack bitfield encoding or channel block budgets) `[OPINION]` |

### 4.2 Critical Findings in `sdk/rocketnet_client_transport.cpp` for Browser Builds

Reading `sdk/rocketnet_client_transport.cpp` and `sdk/rocketnet_driver.h` uncovered two concrete implementation facts that directly govern how Family A (Emscripten WASM) must be built:

1. **The Background Thread Trap (`sdk/rocketnet_client_transport.cpp:355-369`):**
   * In `rocketnet_client_transport_create`, lines `355–369` unconditionally execute:
     ```cpp
     rocketnet_platform_mutex_create( &client_transport->mutex );
     client_transport->thread = rocketnet_platform_thread_create( context, rocketnet_client_thread_function, client_transport );
     ```
     where `rocketnet_client_thread_function` (`lines 1140–1150`) loops until `quit`, calling `rocketnet_client_receive_packets(transport)` and `rocketnet_platform_sleep(0.001)`.
   * **Why this matters in a browser (`[SOURCED: S12, S27, S28, S29, S30, S59]`):** In Emscripten, `pthread_create` requires compiling with `-pthread`, which backs WebAssembly linear memory with a **`SharedArrayBuffer`** (`[SOURCED: S30]`). All browsers (`Chrome 68+`, `Firefox 79+`, `Safari 15.2+`, `[SOURCED: S59]`) block `SharedArrayBuffer` unless the top-level HTML document is served with Cross-Origin Isolation headers (`Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp` or `credentialless`, `[SOURCED: S27, S28, S29]`), and **Android `WebView` does not support `SharedArrayBuffer` at all (`webview_android: false` in MDN BCD `[SOURCED: S59]`)**! Furthermore, requiring COOP/COEP breaks OAuth popups, third-party payment/ad iframes, and web game portals unless every cross-origin subresource sends CORP/CORS headers.
   * **The Fix (8 lines of C++ in `sdk/rocketnet_client_transport.cpp`):** When compiled for single-threaded WebAssembly (`#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)`), skip `rocketnet_platform_thread_create` and call `rocketnet_client_receive_packets(transport)` directly at the start of `rocketnet_client_transport_update()` (`line 546`) before `rocketnet_client_update_swap_receive_buffers(client_transport)` (`line 563`) and `rocketnet_client_process_packet(...)` (`line 582`)! Because the JS transport driver pushes incoming WebTransport/WebRTC/WebSocket datagrams into an in-memory ring buffer asynchronously on the event loop, `io.operations->receive()` is a non-blocking memory copy (`< 100 ns`), so no background C++ thread or `SharedArrayBuffer` is needed at all!
2. **The 15.2 MiB Default Client Memory Footprint (`sdk/rocketnet_client_transport.cpp:29-78, 100-148` & `sdk/rocketnet_fragment.h:119-140`):**
   * `rocketnet_client_transport_t` embeds three huge static arrays sized by `ROCKETNET_FRAME_SIZE` (`2048 B`, `sdk/rocketnet_constants.h:14`) and `ROCKETNET_NUM_CLIENT_PACKETS` (`1024`, `sdk/rocketnet_constants.h:78`):
     * `receive_buffer[2][1024][2048]` = $2 \times 1024 \times 2048 = 4{,}194{,}304\text{ bytes}$ (`4.0 MiB`) (`[ESTIMATED]`)
     * `received_packets[1024][2048]` = $1024 \times 2048 = 2{,}097{,}152\text{ bytes}$ (`2.0 MiB`) (`[ESTIMATED]`)
     * `payload_ring[256]` = $256 \times 1456 = 372{,}736\text{ bytes}$ (`364 KiB`) (`[ESTIMATED]`)
   * And `rocketnet_reassembly_set_create` (`sdk/rocketnet_fragment.cpp:103-141`, called from `sdk/rocketnet_client_transport.cpp:228` with `delivered_capacity = 512` and default `max_payload_bytes = 16384` from `sdk/rocketnet.h:432, 527-528`, `[SOURCED: STANDARD.md:1883-2000, sdk/rocketnet_client_transport.cpp:228, sdk/rocketnet_fragment.cpp:103-141, sdk/rocketnet.h:432, 527-528]`) allocates `set->delivered` (`512 * max_payload_bytes` = `512 * 16384` = $8{,}388{,}608\text{ bytes}$, `8.0 MiB`, `[ESTIMATED]`) plus `set->arena` (`8 * (16384 + 1387)` = $142{,}168\text{ bytes}$, `[ESTIMATED]`).
   * In single-threaded WASM (where `receive_buffer[2]` double-buffering between the background thread and main thread is unnecessary), reducing `ROCKETNET_NUM_CLIENT_PACKETS` from `1024` to `128` for browser builds and hashing delivered payloads (or sizing the delivered ring to `64` entries on the client) shrinks the WASM client heap from **~15.2 MiB to ~1.5 MiB** (`[ESTIMATED]`).

### 4.3 Cryptographic Primitives in the Browser: `libsodium` WASM vs. `WebCrypto` (`SubtleCrypto`)

We verified the current status of the W3C Web Cryptography API (`[SOURCED: S20, S53, S57]`) against RocketNet's required primitives (`[SOURCED: STANDARD.md:38-44, 176-269, sodium/README.md]`):

| RocketNet Primitive (`STANDARD.md`) | Role in RocketNet | In W3C WebCrypto Level 2 (`2025-04-22`, `[S53]`)? | Browser Support in `SubtleCrypto` (`2026-10-14`, `[S57]`) | Recommendation for Browser Client |
| :--- | :--- | :--- | :--- | :--- |
| **`ChaCha20-Poly1305` IETF** (`crypto_aead_chacha20poly1305_ietf`, `[S46, RFC 7539]`) | Encrypts & authenticates every session and handshake packet (`60 Hz`) | **NO** (`0` occurrences in WebCrypto Level 2 `[SOURCED: S53]`) | **None** (`NotSupportedError` in all browsers) `[SOURCED: S53, S57]` | **Must use WASM (`sodium/sodium.c` or `libsodium.js`)**; synchronous, ~1.8 $\mu\text{s}$/packet, zero heap allocations `[OPINION]` |
| **`BLAKE2b-256` / `BLAKE2b-128`** (`crypto_generichash_blake2b`, `[S47, RFC 7693]`) | Derives the 4 directional keys (`RKTNHNDC`, `RKTNHNDS`, `RKTNSESC`, `RKTNSESS`) + token history hash | **NO** (`0` occurrences in WebCrypto Level 2 `[SOURCED: S53]`) | **None** (WebCrypto only has SHA-1/256/384/512, HKDF, PBKDF2) `[SOURCED: S53, S57]` | **Must use WASM (`sodium/sodium.c` or `libsodium.js`)** `[OPINION]` |
| **`X25519`** (`crypto_kx` / `crypto_scalarmult`, `[S49, RFC 7748]`) | Ephemeral handshake key exchange for forward secrecy | **YES** (`§26 X25519` in WebCrypto Level 2 `[SOURCED: S53]`) | **Chrome 133+, Edge 133+, Firefox 130+, Safari 17+, iOS 17+** (`[SOURCED: S57]`) | Since WASM `libsodium` is already loaded for ChaCha20-Poly1305 and BLAKE2b, using `libsodium`'s synchronous X25519 avoids `Promise` async state machines and works on Chrome $< 133$ `[OPINION]` |
| **`Ed25519`** (`crypto_sign_verify_detached`, `[S48, RFC 8032]`) | Server/gateway verifies Connect Token signature (**client never calls Ed25519**, `[SOURCED: STANDARD.md:180-185, 890-893]`) | **YES** (`§25 Ed25519` in WebCrypto Level 2 `[SOURCED: S53]`) | **Chrome 137+, Edge 137+, Firefox 129+, Safari 17+, iOS 17+** (`[SOURCED: S57]`) | Note: Disagrees with the brief's parenthetical that Ed25519 is missing from WebCrypto in current browsers (all 3 engines now ship it as of Chrome 137 / Firefox 129 / Safari 17 `[SOURCED: S57]`), and the RocketNet client does not verify Ed25519 signatures anyway (`[SOURCED: STANDARD.md:892]`). |
| **`SipHash-2-4`** (`crypto_shorthash_siphash24`) | Fast internal hash tables | **NO** (`[SOURCED: S53]`) | **None** (`[SOURCED: S53]`) | Included in `sodium/sodium.c` / `libsodium.js` (`~1 KB` WASM) `[OPINION]` |

### 4.4 Mapping `sdk/rocketnet.h` and `rocketnet_client_driver_operations_t` to JavaScript

The existing client driver seam in `sdk/rocketnet_driver.h:76-119` (`[SOURCED: sdk/rocketnet_driver.h]`) is remarkably clean and requires only **four function pointers**:
```c
struct rocketnet_client_driver_operations_t {
    void * (*create)( void *, const rocketnet_client_driver_config_t *, rocketnet_address_t * );
    void (*destroy)( void * );
    void (*send)( void *, const rocketnet_address_t *, const uint8_t *, int );
    int (*receive)( void *, rocketnet_address_t *, uint8_t *, int );
};
```
* **Zero-Copy Linear Memory Ring Seam (`js_ring_driver`):**
  1. Allocate two fixed ring buffers inside WASM linear memory (`tx_ring[64][1456]` and `rx_ring[128][1456]`) with atomic/volatile `head` and `tail` indices exported to JavaScript via pointers (`rocketnet_wasm_get_rx_ring_ptr()`, `rocketnet_wasm_get_tx_ring_ptr()`).
  2. When the JS transport (`WebTransport` `datagrams.readable` BYOB reader `[SOURCED: S1, §15.7; S55]`, `RTCDataChannel.onmessage` `ArrayBuffer` `[SOURCED: S18]`, or `WebSocket.onmessage` `ArrayBuffer` `[SOURCED: S24]`) receives a packet, JS copies the bytes directly into `HEAPU8.subarray(rx_slot_ptr, rx_slot_ptr + len)` (`single memcpy`) and advances `rx_head`.
  3. On each `requestAnimationFrame` or Web Worker tick (`60 Hz`), the game calls `_rocketnet_client_transport_update(transport)`.
  4. Inside `update()`, `js_ring_driver.receive` pops packets from `rx_ring` into RocketNet (`with *from synthesized to the server's configured rocketnet_address_t so the source-address check in rocketnet_client_protocol.cpp passes!`), processes them, and `js_ring_driver.send` writes outgoing packets into `tx_ring`, which JS flushes immediately to `wt.datagrams`, `rtcDataChannel.send()`, or `ws.send()`.
  5. **Critical detail (`*from` address synthesis):** Because `rocketnet_client_process_packet` in `sdk/rocketnet_client_transport.cpp:402` checks `rocketnet_address_equal(from, &client_transport->protocol.server_address)` on every received packet (`[SOURCED: sdk/rocketnet_client_transport.cpp:402, STANDARD.md:342-392]`), the browser JS driver MUST populate `*from` in `receive()` with the active `server_address` extracted from the Connect Token (`[OPINION]`).

### 4.5 Threading, Workers, Game Engines (Unity WebGL, Blazor, Godot, Bevy, Canvas), and Conformance

* **Where should the RocketNet client run (`Main Thread` vs. `Web Worker` vs. `AudioWorklet`)?**
  * **Default (`Main Thread` or `Dedicated Web Worker` via `postMessage` with transferable `ArrayBuffer`s):** Requires **zero** COOP/COEP headers (`window.crossOriginIsolated === false` `[SOURCED: S27]`). Note that `WebTransport` (`AvailableInWorkers`, `[SOURCED: S15, S16]`) and `WebSocket` (`AvailableInWorkers`, `[SOURCED: S24, S58]`) can be constructed **directly inside a Dedicated Web Worker**, and `RTCDataChannel` is **Transferable to a Web Worker** (`transferable` supported in Chrome 130+, Firefox 144+, Safari 15+ `[SOURCED: S56]`)! Running the entire RocketNet WASM/TS client + transport inside a Dedicated Web Worker prevents main-thread rendering or GC pauses from delaying 10 Hz keep-alives or 60 Hz packet processing (`[OPINION]`).
  * **Why NOT `AudioWorklet` (`[SOURCED: S25]`):** While `AudioWorkletGlobalScope` runs on a high-priority audio render thread (`128`-frame quanta $\approx 2.67\text{ ms}$ at 48 kHz), it has **no network APIs (`WebTransport`, `WebSocket`, `RTCDataChannel` are all unavailable in `AudioWorkletGlobalScope`)** and blocks until a user gesture resumes the `AudioContext` (`[SOURCED: S25]`).
* **Game Engine Consumption (`[SOURCED: csharp/README.md, csharp/RocketNet.csproj]`):**
  * **Unity WebGL & Blazor WebAssembly (`csharp/`):** The repository already ships a C# P/Invoke binding (`csharp/RocketNet.cs`, `[DllImport("rocketnet")]`, `[SOURCED: csharp/README.md]`). For Unity WebGL (IL2CPP $\rightarrow$ Emscripten WASM) and Blazor WASM (`NativeFileReference`), compiling `librocketnet` to a static `.a` archive with `emcc` and linking it into the Unity/Blazor WASM binary (with `[DllImport("__Internal")]` on `UNITY_WEBGL && !UNITY_EDITOR`) allows existing C# game code to call `RocketNetClientTransport` **unchanged**!
  * **Bevy (Rust `wasm32-unknown-unknown`), Godot 4 (Web export), and Plain HTML5 Canvas / WebGPU (`[SOURCED: S32]`):** Bevy and Godot can either link the C ABI statically into their WASM module or consume the TypeScript/WASM ES module via `wasm-bindgen` / Godot `JavaScriptBridge`. Plain Canvas / Three.js / Babylon.js / WebGPU games import the `@mas-bandwidth/rocketnet-browser` ES module directly.
* **Keeping Conformance (`STANDARD.md` Appendix A Known-Answer Vectors):**
  * `STANDARD.md` Appendix A (`lines 3804–4133`) and `sdk/rocketnet_test_vectors.h` define deterministic Known-Answer Test (KAT) vectors with fixed keys, salts, nonces, and exact expected wire bytes for all packet types (`Types 0–9`), cookie HMAC, key derivation (`RKTNHNDC`, `RKTNHNDS`, `RKTNSESC`, `RKTNSESS`), replay protection, and fragmentation (`[SOURCED: STANDARD.md:3804-4133, sdk/rocketnet_test_vectors.h]`).
  * Both Family A (WASM C++) and Family B (TypeScript) MUST run the full Appendix A KAT suite in headless Chrome, Firefox, and WebKit (Playwright) on every CI run, asserting byte-for-byte equality on every vector (`[OPINION]`).

---

## 5 Measurement plan

Nothing is executed in this research card; this section specifies the exact, reproducible measurement harness to run on the testbed machines so the transport and gateway decisions are governed by measured percentiles rather than guesswork.

### 5.1 Testbed Topology, Machines, and Browsers

* **Topology:** Two physical machines connected over a switched 1 GbE / 10 GbE LAN (baseline RTT $< 0.3\text{ ms}$) with a Linux traffic-shaping bridge (`tc qdisc add dev eth0 root netem ...`) inline between them (`[OPINION]`):
  * **Client Machine (macOS Apple Silicon & Mobile Device Harness):** Runs headless and headed browser instances plus an attached iPhone (iOS 26.4+ via USB/Wi-Fi) and Android phone (Android 15/16 via `adb`):
    * **Chrome 157** (Desktop macOS & Android)
    * **Edge 154** (Desktop)
    * **Firefox 160** (Desktop macOS & Android)
    * **Safari 26.4+** (Desktop macOS & iOS 26.4+) and **Safari 18.x / 26.3** (to test the pre-WebTransport WebRTC fallback path)
  * **Server Machine (x86_64 / ARM64 Linux Kernel 6.x):** Runs the native C++ `rocketnet` server pinned to core `0` (`taskset -c 0`) and the Browser Gateway (`WebTransport`, `WebRTC ICE-Lite`, `WebSocket`, `Striped WebSocket`) pinned to cores `1..N`.

### 5.2 Timestamping and Sample Size for $p_{99}$ and $p_{99.9}$

* **Clock & Timestamping Method:**
  * One-way latency between two machines without hardware PTP (`IEEE 1588`) suffers from clock offset/skew. We therefore measure **both**:
    1. **Echo RTT & Asymmetric Split:** Browser stamps `t0_us = Math.round(performance.timeOrigin * 1000 + performance.now() * 1000)` (`[SOURCED: S31]`, note: with COOP/COEP enabled in the benchmark harness, `performance.now()` has **5 $\mu\text{s}$ resolution** vs. 100 $\mu\text{s}$ without cross-origin isolation `[SOURCED: S27, S31]`) into bytes `0..8` of the RocketNet payload; the native server stamps its `CLOCK_MONOTONIC_RAW` nanosecond timestamp into bytes `8..16` and echoes the payload immediately; the browser records `t1_us` on receipt.
    2. **Same-Host Loopback / Hardware-Synced One-Way Latency:** When running browser and server across two NICs on the same host (through a `veth` / namespace `netem` bridge), both browser (`performance.now()`) and native server share the same hardware TSC (`CLOCK_MONOTONIC`), giving exact microsecond one-way delay (`OWD = t_recv - t_send`).
* **Sample Size for $p_{99}$ and $p_{99.9}$:**
  * By order-statistic binomial confidence intervals, estimating $p_{99.9}$ with a tight 95% confidence interval requires at least **50,000–100,000 samples** per matrix cell ($\ge 50\text{–}100$ tail events above $p_{99.9}$).
  * At **60 Hz**, `72,000 packets` takes `20 minutes` per single-client run, or **`60 seconds` across `20 concurrent browser tabs / workers`** (`20 * 60 * 60 = 72,000 packets` `[ESTIMATED]`).

### 5.3 Test Matrix

1. **Send Rate $\times$ Payload Size Sweep (Baseline & Shaped):**
   * **Tick rates:** `10 Hz` (100 ms), `30 Hz` (33.33 ms), `60 Hz` (16.67 ms).
   * **Payload sizes:** `100 B`, `400 B`, `800 B`, `1120 B` (QUIC initial-MTU safe), `1192 B` (`ROCKETNET_MAX_PAYLOAD_BYTES` `[SOURCED: sdk/rocketnet.h:420]`).
   * **Metrics recorded:** OWD and RTT `p50`, `p90`, `p99`, `p99.9`, `max`, inter-arrival jitter (RFC 3550), and effective application delivery rate.
2. **Impaired Network Matrix (`tc netem`):**
   * **Uniform loss:** `0%`, `1%`, `5%`, `10%` (`tc qdisc change dev veth0 root netem delay 25ms loss random X%`).
   * **Base RTT (`2 * one-way delay`):** `20 ms`, `50 ms`, `100 ms` (`netem delay 10ms`, `25ms`, `50ms`).
   * **Jitter & Reordering:** `delay 25ms 10ms distribution normal` and `reorder 5% 50%`.
   * **Burst Loss ( Gilbert-Elliott 2-state Markov model):** `loss gemodel 2% 25%` (average burst of 4 consecutive lost packets) to test whether QUIC or SCTP congestion control stalls subsequent ticks after a burst loss.
   * **Rate Limits (Bufferbloat Test):** `tc qdisc ... tbf rate 2mbit burst 16kbit latency 50ms` with a concurrent HTTP download in the same browser tab to measure cross-traffic bufferbloat between WebTransport (`allowPooling: false`), WebRTC, and WebSocket.
3. **WebSocket Head-of-Line (HoL) Blocking Quantification:**
   * Measure **$\Delta\text{OWD}(p_{99}) = \text{OWD}_{\text{transport}}(p_{99}) - \text{OWD}_{\text{native UDP}}(p_{99})$** and **Tick Inter-Arrival Gap $p_{99}$ / $p_{99.9}$** at 60 Hz across `0%`, `1%`, `5%`, `10%` loss for single WebSocket vs. Striped WebSocket ($K=3$) vs. WebTransport vs. WebRTC.
4. **Connection Setup Time (Time-to-First-RocketNet-KeepAlive):**
   * Cold-start connection setup (including DNS/TLS/ICE/DTLS/SCTP + RocketNet 3-RTT handshake, and 2-RTT gateway-cookie-prefetched handshake) at `20 ms`, `50 ms`, and `100 ms` RTT (`N = 500` trials per transport).
5. **Server Scale (PPS/Core & Memory/Client) and Mobile Browser CPU/Battery:**
   * **Server scale:** Synthetic load generator spawning `500`, `1,000`, `2,500`, `5,000` concurrent sessions at 60 Hz bidirectional (`120 pkt/s/session` = up to `600,000 pps`), recording gateway + native server CPU utilization (`perf stat`), context switches, and RSS/PSS (`smem` / `/proc/pid/smaps_rollup`) per client.
   * **Phone CPU & Battery:** 15-minute 60 Hz session on iPhone (Safari 26.4 over 5G & Wi-Fi) and Android phone (Chrome 154) measuring Web Inspector CPU timeline (`% main thread`, `% worker thread`) and `BatteryManager` / Xcode Energy Log drain.
   * **NAT Types & Cellular Handoff:** Test across Full Cone, Address-Restricted, Port-Restricted, and **Symmetric NAT** (`iptables` MASQUERADE `--random-fully`) plus live Wi-Fi $\rightarrow$ 5G interface toggle.

### 5.4 Quantitative Pass/Fail Decision Lines (`[OPINION]`)

* **Primary Transport Pass Line (WebTransport vs. WebRTC):** WebTransport wins primary transport if at `60 Hz`, `50 ms` RTT, `1%` packet loss:
  1. $\text{OWD}(p_{99}) \le \text{OWD}_{\text{native UDP}}(p_{99}) + 3.0\text{ ms}$ and $\text{OWD}(p_{99.9}) \le \text{OWD}_{\text{native UDP}}(p_{99.9}) + 8.0\text{ ms}$, **and**
  2. Cold-start connection setup (through first RocketNet keep-alive) $p_{95} \le 4.0 \times \text{RTT}$ ($\le 200\text{ ms}$ at `50 ms` RTT, compared to $\ge 350\text{ ms}$ for WebRTC), **and**
  3. Gateway CPU per 1,000 clients at 60 Hz is $\le 1.5$ cores (`>= 80,000 pps/core`).
* **Overturn Line (When WebRTC Beat WebTransport):** If QUIC congestion control in Chrome/Safari stalls outgoing WebTransport datagrams during `5%` loss or Gilbert-Elliott burst loss such that $\text{OWD}(p_{99})$ exceeds WebRTC `RTCDataChannel` (`ordered: false, maxRetransmits: 0`) by $> 10\text{ ms}$, WebRTC becomes primary even on WebTransport-capable browsers.

---

## 6 Support matrix

### 6.1 Browser $\times$ Platform $\times$ Transport Support Table (Read 2026-10-14)

All version numbers below were read on `2026-10-14` from `caniuse` (`[SOURCED: S9, S10, S11, S60, S61, S62]`) and MDN `browser-compat-data` (`[SOURCED: S54, S55, S56, S57, S58, S59]`).

| Browser / Runtime & Platform | Engine | WebTransport (`HTTP/3` Datagrams) | WebTransport `serverCertificateHashes` | WebRTC `RTCDataChannel` (`ordered: false, maxRetransmits: 0`) | WebSockets (`wss://`) | `SharedArrayBuffer` (Requires COOP/COEP) | WebCrypto `X25519` / `Ed25519` |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **Chrome (Windows, macOS, Linux)** | Blink / V8 | **Yes (`97+`)** (`datagrams.writable`) `[S54, S60]` | **Yes (`100+`)** `[S54]` | **Yes (`30+`, `maxPacketLifeTime` `56+`)** `[S56]` | **Yes (`16+`)** `[S58, S62]` | **Yes (`68+`)** `[S59]` | **Yes (`133+` / `137+`)** `[S57]` |
| **Edge (Windows, macOS, Linux)** | Blink / V8 | **Yes (`98+`)** `[S60]` | **Yes (`100+`)** `[S54]` | **Yes (`79+`)** `[S56, S61]` | **Yes (`12+`)** `[S58, S62]` | **Yes (`79+`)** `[S59]` | **Yes (`133+` / `137+`)** `[S57]` |
| **Firefox (Windows, macOS, Linux)** | Gecko / SpiderMonkey | **Yes (`114+`)** (both `writable` & `createWritable` in `155+`) `[S54, S55]` | **Yes (`125+`)** `[S54]` | **Yes (`22+`, `maxRetransmits` `62+`)** `[S56]` | **Yes (`11+`)** `[S58, S62]` | **Yes (`79+`)** `[S59]` | **Yes (`130+` / `129+`)** `[S57]` |
| **Safari (macOS)** | WebKit / JavaScriptCore | **Yes (`26.4+`, March 2026)** (`createWritable()` only; **No** on $\le 26.3$) `[S54, S55, S60]` | **Yes (`26.4+`)** `[S54]` | **Yes (`11+`)** `[S56, S61]` | **Yes (`7+`)** `[S58, S62]` | **Yes (`15.2+`)** `[S59]` | **Yes (`17+` / `17+`)** `[S57]` |
| **Chrome on Android** | Blink / V8 | **Yes (`97+` / `154`)** `[S54, S60]` | **Yes (`100+`)** `[S54]` | **Yes (`30+` / `154`)** `[S56, S61]` | **Yes (`16+` / `154`)** `[S58, S62]` | **Yes (`89+`)** `[S59]` | **Yes (`133+` / `137+`)** `[S57]` |
| **Firefox on Android** | Gecko / SpiderMonkey | **Yes (`114+` / `157`)** `[S54, S60]` | **Yes (`125+`)** `[S54]` | **Yes (`24+` / `62+`)** `[S56]` | **Yes (`11+` / `157`)** `[S58, S62]` | **Yes (`79+`)** `[S59]` | **Yes (`130+` / `129+`)** `[S57]` |
| **Safari on iOS & iPadOS** | WebKit | **Yes (`26.4+`)**; **No on iOS $\le 26.3$** `[S54, S60]` | **Yes (`26.4+`)** `[S54]` | **Yes (`11.0+`)** `[S56, S61]` | **Yes (`6.0+`)** `[S58, S62]` | **Yes (`15.2+`)** `[S59]` | **Yes (`17.0+`)** `[S57]` |
| **Chrome / Edge / Firefox / Brave on iOS (Non-EU & Default)** | **WebKit (`WKWebView`)** | **Same as iOS Safari (`26.4+` only)** (see note below) `[S54, S60]` | **Yes (`26.4+`)** `[S54]` | **Yes (`11.0+`)** `[S56]` | **Yes (`6.0+`)** `[S58]` | **Yes (`15.2+`)** `[S59]` | **Yes (`17.0+`)** `[S57]` |
| **Electron / Steam Overlay (`CEF`)** | Chromium (Blink) | **Yes** (Chromium 97+ / Electron 17+) `[S54]` | **Yes** (Chromium 100+) `[S54]` | **Yes** `[S56]` | **Yes** `[S58]` | **Yes** `[S59]` | **Yes** (Chromium 137+) `[S57]` |
| **Android `WebView` (Capacitor / Cordova / In-App)** | System Blink WebView | **Yes (`97+`)** (`local_network_access: false` `[S54]`) | **Yes (`100+`)** `[S54]` | **Yes** `[S56]` | **Yes** `[S58]` | **NO (`webview_android: false`)** `[SOURCED: S59]` | **Yes (`133+` / `137+`)** `[S57]` |
| **iOS `WKWebView` (Capacitor / Unity WebGL in iOS App / Discord/TikTok In-App)** | System WebKit | **iOS `26.4+` only** (`false` on iOS $\le 26.3$) `[S54, S60]` | **iOS `26.4+` only** `[S54]` | **Yes** (no camera/mic prompt for DataChannel-only!) `[S56]` | **Yes** `[S58]` | **Yes (`15.2+` if COOP/COEP served)** `[S59]` | **Yes (`17.0+`)** `[S57]` |

* **What "All iOS browsers use WebKit" means in practice:** Outside specific EU Digital Markets Act alternative browser-engine entitlements, every third-party browser on iOS and iPadOS (Chrome for iOS, Firefox for iOS, Edge for iOS, Brave, Discord/Instagram/TikTok in-app browsers) is built on Apple's system **`WKWebView` (WebKit)**. Consequently:
  1. On any iPhone or iPad running iOS $\le 26.3$, **neither Safari nor Chrome-on-iOS nor Firefox-on-iOS supports `WebTransport`** (`[SOURCED: S54, S60]`). Those devices **must** use `WebRTC RTCDataChannel` (which has worked in `WKWebView` since iOS 11) or `WebSockets`.
  2. On iOS $26.4+$, `WebTransport` works across Safari and `WKWebView`, **provided** the client calls `wt.datagrams.createWritable()` rather than relying on `wt.datagrams.writable` (`[SOURCED: S55]`).

### 6.2 Enterprise, School, and Mobile Network Blocking Matrix

| Network Environment | Raw UDP (High Ports `10000–65535`) | UDP Port `443` (QUIC / HTTP/3 / WebTransport) | WebRTC (`ICE-Lite` on UDP/4443 or UDP/443) | WebRTC via `TURN/TLS` on TCP/443 | WebSockets (`wss://` on TCP/443) |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Residential Fiber / Cable / DSL (NAT)** | Allowed (~95%) | Allowed (~95%) | Allowed (~96%) | Allowed (>99%) | Allowed (>99.9%) |
| **Mobile Cellular (4G LTE / 5G CGNAT, Carrier IPv6)** | Allowed (~90–95%; strict port-aging timeouts of 15–30s) | Allowed (~92–96%) | Allowed (~95%; ICE handles CGNAT) | Allowed (>99%) | Allowed (>99.9%) |
| **Campus / University / Hotel Wi-Fi** | Often blocked (~30–50% block arbitrary UDP) `[UNVERIFIED exact %]` | Sometimes blocked (~15–25% block UDP/443 to force HTTP/2 TCP) `[UNVERIFIED exact %]` | Blocked on high UDP ports; **works if gateway ICE-Lite listens on UDP/443 or UDP/3478** | Allowed (~98%) | Allowed (~99%) |
| **Enterprise / K-12 School Firewall (UDP Blocked)** | **Blocked** | **Blocked** | **Blocked** (unless TURN over TCP/443 is configured) | Allowed (~90%) | **Allowed (~95–99%)** |
| **Enterprise TLS-Inspecting Proxy (MITM Middlebox)** | **Blocked** | **Blocked** (middleboxes block UDP/443 so they can MITM TLS over TCP/443) | **Blocked on UDP** | Often blocked if non-HTTP ALPN | **Works via `HTTP CONNECT` proxy** (unless `Upgrade: websocket` is explicitly stripped) |

* **Player Reach Share Figures:**
  * Note: The `Fyrd/caniuse` feature JSON files (`webtransport.json`, `rtcpeerconnection.json`, `websockets.json` `[SOURCED: S60, S61, S62]`) leave `usage_perc_y: null` in the raw feature definitions because CanIUse computes global share dynamically by joining browser version tables against regional StatCounter tables in the client bundle (`[SOURCED: S9, S60]`). Exact global player-weighted share figures are therefore marked **`[UNVERIFIED]`** where external StatCounter tables could not be joined directly, with conservative bounds derived from the browser version tables:
    * **WebSockets (`wss://`):** **~99%+ of active browsers** (`W3C Baseline High since 2018`, supported in every browser version released in the last 10+ years `[SOURCED: S11, S62]`, exact network-reachable share `[UNVERIFIED]`).
    * **WebRTC `RTCDataChannel`:** **~96–98% of active browser engines** (`W3C Baseline High since 2022`, supported in all desktop and mobile browsers since 2017 `[SOURCED: S10, S61]`), reaching **~88–94% of connections over direct UDP** (exact firewall drop rate `[UNVERIFIED]`).
    * **WebTransport (`HTTP/3`):** Supported in 100% of current Chrome/Edge (`97+`), Firefox (`114+`), and Safari (`26.4+`, released March 2026 `[SOURCED: S9, S54, S60]`), but limited on iOS/macOS users who have not yet upgraded to 26.4+ and on networks blocking UDP/443 (**estimated ~75–88% effective reach today**, exact global share `[UNVERIFIED]`).
  * **Conclusion:** A browser game **cannot** ship WebTransport-only or WebRTC-only without losing 6–20% of players on older iOS versions or UDP-restricted school/enterprise networks. Supporting the **3-tier cascade (WebTransport $\rightarrow$ WebRTC $\rightarrow$ WebSocket)** reaches the full ~99%+ browser audience (`[OPINION]`).

---

## 7 Ideas of your own

Each entry below is an independent design proposal marked as **`idea`**.

1. **`idea` — Happy-Eyeballs Transport Cascade with Gateway Session Handover (`250 ms` Staggered Race):**
   * Modeled on RFC 8305 Happy Eyeballs v2 (`[SOURCED: S50]`), the browser client starts **WebTransport** at $t = 0\text{ ms}$ (since it takes only `1 RTT` when available). If `wt.ready` has not resolved by $t = 200\text{ ms}$ (or if `typeof WebTransport === "undefined"`), it launches **WebRTC (ICE-Lite)** and **Striped WebSockets** in parallel.
   * **Even better (Warm Upgrade):** Because WebSockets connects in `2 RTTs` while WebRTC takes `5–6 RTTs`, the client can begin the RocketNet 3-RTT handshake immediately over WebSocket at $t = 0\text{ ms}$ so the player enters the game in `< 150 ms`, while WebRTC negotiates in the background! Once the WebRTC `RTCDataChannel` opens, the client sends a signed 16-byte handover frame (authenticated with a subkey derived from `RKTNSESC` or the next RocketNet packet itself!) over the new WebRTC channel; the gateway rebinds that session's pointer to the **existing** `udp_fd[i]` local UDP socket! The native RocketNet server sees the exact same `(127.0.0.1, port_i)` identity and never drops the connection while the browser seamlessly upgrades from TCP to unreliable UDP mid-match!
2. **`idea` — Gateway-Assisted Loopback Cookie Splicing (3-RTT $\rightarrow$ 2-RTT Wide-Area Handshake without Changing `STANDARD.md`):**
   * As detailed in `§3.3`, bytes `6..30` (`return_cookie`) of the 710-byte `Type 0 Connection Request` are outside the Ed25519 signature (`[SOURCED: STANDARD.md:1204-1225]`). Because the gateway has already validated the browser's transport address via QUIC or TCP/DTLS, the gateway can perform the stateless `Type 0 (empty cookie)` $\rightarrow$ `Type 7 (38B cookie)` $\rightarrow$ `Type 0 (with cookie)` exchange against the co-located native server over `127.0.0.1` in `< 150 us` on the very first browser packet, shaving **1 full wide-area RTT (20–100 ms)** off every browser connection setup with zero changes to the native server!
3. **`idea` — Carrying `serverCertificateHashes` Inside the Connect Token's 64-Byte Trailing Zero Pad (`STANDARD.md:844, 866-869, 882`):**
   * `STANDARD.md` specifies a fixed 768-byte Connect Token where bytes `704..768` (`64 bytes`) are zero-padding outside the Ed25519 signature (and bytes `521..576` are `55 bytes` of zero-padding inside the signed data). Storing the gateway's 32-byte SHA-256 certificate digest (`serverCertificateHashes`) + 2-byte WebTransport port + 2-byte WebRTC port inside the signed 55-byte pad (or a v1.1 token extension) binds the WebTransport ephemeral TLS certificate directly to the backend's Ed25519 signature! That eliminates TOFU/MITM risks and lets a single 768-byte token bootstrap native UDP, WebTransport, WebRTC, and WebSockets alike.
4. **`idea` — Shared-Memory Ring Driver (`rocketnet_shm_driver`) Between Gateway and Native Server:**
   * Instead of sending packets over `127.0.0.1` UDP sockets between the co-located Browser Gateway and the native RocketNet server (`~10–15 us` kernel syscall overhead per packet), use `rocketnet_server_driver_operations_t` (`[SOURCED: sdk/rocketnet_driver.h:35-70]`) to provide a lock-free SPSC/MPSC shared-memory (`shm_open` / `mmap`) driver alongside the native UDP driver! Each browser session $i$ gets a synthetic `rocketnet_address_t` (`IPv6 ::ffff:127.0.0.1`, `port = i`), zero kernel sockets are consumed per browser client, and gateway-to-server handoff latency drops from `~15 us` to **`< 200 ns`**!
5. **`idea` — Striped Multi-WebSocket ($K=3$) + `TCP_NOTSENT_LOWAT` Fallback for Zero-HoL Corporate/School Play:**
   * Rather than treating WebSockets as a degraded, laggy fallback, open $K=3$ parallel `wss://` sockets bound to the same gateway session `udp_fd[i]` and dispatch each outgoing tick onto the socket with the lowest `bufferedAmount` (`=== 0`). Because RocketNet's sequence number and 4096-packet replay bitmap natively handle out-of-order packets (`[SOURCED: STANDARD.md:1533-1703]`), a lost TCP segment on socket 0 only stalls socket 0 while sockets 1 and 2 carry subsequent 60 Hz ticks without waiting for the TCP retransmission!
6. **`idea` — Single-Threaded Emscripten Mode + Browser-Sized Memory Profile (`15.2 MiB` $\rightarrow$ `1.2 MiB`):**
   * Add a compile-time `ROCKETNET_BROWSER_CLIENT` profile to `sdk/rocketnet_client_transport.cpp` that (a) drains `rocketnet_client_receive_packets` synchronously at the start of `rocketnet_client_transport_update` (eliminating `pthread_create`, `SharedArrayBuffer`, and COOP/COEP headers), and (b) sizes `ROCKETNET_NUM_CLIENT_PACKETS` to `128` instead of `1024`, cutting per-tab WASM linear memory by **90% (`15.2 MiB` $\rightarrow$ `~1.5 MiB`)**.
7. **`idea` — Dedicated Worker + `OffscreenCanvas` / `WebGPU` Isolation:**
   * Run the RocketNet client inside a Dedicated Web Worker that owns the `WebTransport` / `RTCDataChannel` / `WebSocket` instance and sends compact decoded game state snapshots (or shares an `OffscreenCanvas` / WebGPU render loop `[SOURCED: S26, S32]`), guaranteeing that main-thread DOM layout or garbage collection never causes a 5-second handshake timeout or keep-alive drop.
8. **`idea` — Mobile Page Lifecycle (`visibilitychange` / `pagehide`) Grace Window & Fast Reconnect:**
   * Mobile browsers on iOS and Android aggressively freeze backgrounded tabs (`visibilitychange` $\rightarrow$ `"hidden"`), stopping JS timers within 1–5 seconds. Since RocketNet's development token `timeout_seconds` is `10 seconds` (`signed_fields.timeout_seconds = 10` at `sdk/rocketnet_connect_token.cpp:216`, test timeout `10` at `sdk/rocketnet_test_vectors.h:22`, server config default `max_timeout_seconds = 60` at `sdk/rocketnet.h:730`, `[SOURCED: STANDARD.md:2223, sdk/rocketnet_connect_token.cpp:216, sdk/rocketnet_test_vectors.h:22, sdk/rocketnet.h:730]`), the co-located gateway can send synthetic keep-alives on behalf of a backgrounded mobile browser session for up to a configured grace window (`e.g. 15–30s`) when the client sends a `"tab-hidden"` hint before suspension, preventing mobile players from being kicked when briefly switching apps or dismissing a notification.
9. **`idea` — Browser-Specific Anti-Cheat & Bot Hardening (Server-Authoritative Inputs + Rate Envelopes):**
   * In a browser, DevTools allows any user to hook `WebTransport`, `RTCDataChannel.prototype.send`, or WASM linear memory (`WebAssembly.Memory.prototype.buffer`). Never trust client-side state or physics in a browser build: keep the RocketNet cryptographically sealed channel strictly for **server-authoritative input commands** and enforce strict per-slot packet-rate and byte-rate ceilings in the gateway (dropping floods before they even reach localhost).
10. **`idea` — Zero-Server-CPU Spectator Mode & Instant Browser Replays via WebTransport Broadcast:**
    * Because RocketNet's `docs/CHANNELS.md` state updates are deterministic records inside `Packet Type 5` payloads, the co-located Browser Gateway can tap a single read-only spectator slot on the native server, decrypt or receive the spectator broadcast feed, and fan it out to **10,000+ browser spectators** over WebTransport datagrams/streams (`[SOURCED: S1]`) or save the compact stream into browser `OPFS` (Origin Private File System) for instant in-browser match replays!

---

## 8 Ranked R&D experiments

### 8.1 Ranking Formula and Summary Table

Every experiment below is ranked by:
$$\text{Score} = \frac{\text{Decision Value (1–10 scale for choosing primary transport, fallback, and server-vs-gateway)}}{\text{Cost in Worker-Days}}$$
Putting the **cheapest decisive experiments first**, starting with **`E1`** (a 0.5-day Monday-ready experiment on macOS where a browser page exchanges real RocketNet packets with the unmodified native C++ server), and including **three explicit overturn experiments (`E5`, `E6`, `E8`)** designed to falsify our own recommendations (`WebTransport primary`, `Co-located Gateway over In-Process Embedding`, and `End-to-End AEAD over Gateway Termination`).

| Rank & ID | Experiment Title | Topic | Value (1–10) | Cost (Worker-Days) | Value / Cost | Overturns Our Recommendation If... |
| :--- | :--- | :--- | :---: | :---: | :---: | :--- |
| **1 (`E1`)** | **Monday First Rung:** Browser Page $\leftrightarrow$ Local Gateway Shim $\leftrightarrow$ Native C++ Server on macOS (`Zero Server Edits`) | `§1, §2, §3, §4` | `10` | **`0.5 d`** | **`20.0`** | Foundational proof of the zero-server-change gateway + WASM client architecture |
| **2 (`E2`)** | Single-Threaded Emscripten WASM Client (`sdk/` + `sodium.c`) Running `STANDARD.md` Appendix A KAT Vectors in Browser | `§4 Client library` | `9` | **`0.5 d`** | **`18.0`** | Binary size $> 250\text{ KB}$ gzipped or per-packet crypto $> 25\ \mu\text{s}$ |
| **3 (`E3`)** | Cross-Browser `WebTransport` + `serverCertificateHashes` Matrix (`Chrome 157`, `Firefox 160`, `Safari 26.4` `createWritable()`) | `§1, §6 Support` | `9` | **`0.5 d`** | **`18.0`** | Safari 26.4 rejects `serverCertificateHashes` or drops datagrams $> 1171\text{ B}$ |
| **4 (`E4`)** | Gateway-Assisted Loopback Cookie Prefetch (Collapsing 3-RTT Handshake to 2 WAN RTTs with Zero Server Changes) | `§3 Protocol mapping` | `8` | **`0.5 d`** | **`16.0`** | Native server rejects cookie-spliced `Type 0` request |
| **5 (`E5`)** | **[Overturn Test #1]** Head-to-Head Latency & Jitter under `netem` Loss (`0, 1, 5, 10%` & Burst): WebTransport vs. WebRTC vs. Striped WS | `§1, §5 Measurement` | `10` | **`1.0 d`** | **`10.0`** | **Overturns WebTransport as Primary:** QUIC cwnd stalls datagrams under 5% loss worse than WebRTC `maxRetransmits: 0` |
| **6 (`E6`)** | **[Overturn Test #2]** End-to-End RocketNet AEAD vs. Gateway-Terminated AEAD (Browser CPU, Battery & Gateway Throughput) | `§3 Protocol mapping` | `8` | **`1.0 d`** | **`8.0`** | **Overturns End-to-End AEAD:** Double encryption costs $> 15\%$ gateway CPU or $> 5\%$ mobile frame time |
| **7 (`E7`)** | Striped Multi-WebSocket ($K=1, 2, 3, 4$) + `TCP_NOTSENT_LOWAT` Head-of-Line Blocking Mitigation Under Loss | `§1, §5 Measurement` | `8` | **`1.0 d`** | **`8.0`** | Striping $K=3$ fails to reduce $p_{99}$ HoL delay by $\ge 50\%$ at 2% loss |
| **8 (`E8`)** | **[Overturn Test #3]** Co-Located UDP Gateway vs. In-Process Sans-I/O `ngtcp2`/`quiche` + Shared-Memory Driver (`XDP/GSO` Coexistence) | `§2 Server side` | `9` | **`1.5 d`** | **`6.0`** | **Overturns Sidecar Gateway:** Loopback UDP hop adds $> 100\ \mu\text{s}$ p99 or caps under `50k pps/core` |
| **9 (`E9`)** | Pure TypeScript + `libsodium.js` Client vs. Emscripten C++ WASM Client (Bundle Size, Memory Footprint & `CHANNELS.md` Parity) | `§4 Client library` | `8` | **`1.5 d`** | **`5.3`** | TS bundle isn't $\ge 40\%$ smaller or diverges on `CHANNELS.md` edge cases |
| **10 (`E10`)** | Happy-Eyeballs Transport Cascade + Seamless Mid-Match Warm Upgrade (`WebSocket` $\rightarrow$ `WebTransport`/`WebRTC`) | `§1, §7 Ideas` | `8` | **`1.5 d`** | **`5.3`** | Handover drops $> 1$ tick or triggers replay/address rejection |
| **11 (`E11`)** | 2,500-Client Server Scale & Memory Benchmark (`WebTransport` vs. `WebRTC ICE-Lite UDP-Mux` vs. `uWebSockets` at 60 Hz) | `§2, §5 Measurement` | `9` | **`2.0 d`** | **`4.5`** | WebRTC or WT gateway exceeds `150 KiB/client` or `< 40k pps/core` |
| **12 (`E12`)** | Mobile Cellular (`5G` $\leftrightarrow$ `Wi-Fi` Migration) & `visibilitychange` Background Survival on iOS 26.4 and Android | `§3, §6, §7` | `7` | **`1.5 d`** | **`4.7`** | Network switch drops session despite stable gateway loopback port |

---

### 8.2 Detailed Experiment Specifications (`E1` through `E12`)

#### E1: Monday First Rung — Browser Page Exchanges RocketNet Packets with Native C++ Server on macOS
* **ID:** `E1` (Rank 1, Score `20.0`)
* **Topic:** `§1 Transports`, `§2 Server side`, `§3 Protocol mapping`, `§4 Client library`
* **Question it answers:** Can a browser page on macOS complete the full RocketNet Connect Token admission, 3-RTT handshake, ChaCha20-Poly1305 encrypted session, and 60 Hz bidirectional `Packet Type 5` payload exchange with an **unmodified** native C++ `rocketnet` server binary via a per-session UDP gateway shim and single-threaded Emscripten WASM client?
* **Hypothesis:** Replacing the background receive thread in `sdk/rocketnet_client_transport.cpp` under `#ifdef __EMSCRIPTEN__` with an inline receive drain in `rocketnet_client_transport_update()` and plugging in a 60-line JS ring-buffer `rocketnet_client_driver_operations_t` over `WebSocket` (and `WebTransport`) will complete the handshake in $< 50\text{ ms}$ on localhost and exchange 60 Hz encrypted payloads with zero server modifications.
* **What would be built (Step-by-step Monday plan for a worker on macOS):**
  1. **Native Server (`15 min`):** Compile the existing native server example/test on macOS (`cmake -B build && cmake --build build`) listening on `127.0.0.1:40000` with a test `server_secret_key` / `backend_public_key`, and mint a 768-byte Connect Token using `rocketnet_write_dev_connect_token` (`sdk/rocketnet.h:541`) / `rocketnet_write_connect_token` (`sdk/rocketnet_connect_token.h:87`).
  2. **Single-Threaded WASM Client (`90 min`):** In `sdk/rocketnet_client_transport.cpp`, guard `rocketnet_platform_thread_create` with `#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)` and call `rocketnet_client_receive_packets(transport)` at the top of `rocketnet_client_transport_update()`. Write a 60-line C driver `wasm_ring_driver.c` implementing `rocketnet_client_driver_operations_t` backed by two ring buffers (`rx_ring`, `tx_ring`) in WASM linear memory. Compile `sdk/rocketnet_*.cpp` + `sodium/sodium.c` + `wasm_ring_driver.c` with `emcc -O2 -sMODULARIZE=1 -sEXPORT_ES6=1 -o rocketnet_wasm.js`.
  3. **Local Gateway Shim (`45 min`):** Write a 90-line Go or C++ gateway listening on `ws://127.0.0.1:8080/rn` (and `https://127.0.0.1:8443/rn` for WebTransport). On each browser connection, allocate an ephemeral UDP socket `bind(127.0.0.1:0)` + `connect(127.0.0.1:40000)` and pipe frames bidirectionally.
  4. **Browser Test Page (`30 min`):** Serve `index.html` on `http://127.0.0.1:8080` that loads `rocketnet_wasm.js`, passes the 768-byte Connect Token to `_rocketnet_client_transport_connect()`, ticks `_rocketnet_client_transport_update()` at 60 Hz via `requestAnimationFrame`, and sends/receives 1000 encrypted 256-byte payloads.
* **Measurement and machine:** Run on a macOS workstation with Chrome 157 and Safari 26.4; measure handshake completion time (`ms`), 1000-packet delivery ratio, and browser `performance.now()` CPU time per `update()` call.
* **Cost in worker-days:** `0.5 worker-days` (4 hours).
* **Dead-end criterion:** If single-threaded `rocketnet_client_transport_update()` fails to complete the 3-RTT handshake without `-pthread` / `SharedArrayBuffer`, or if `update()` exceeds `1.0 ms` per frame on macOS.
* **What it unblocks:** Unblocks all browser transport benchmarks (`E3`, `E4`, `E5`, `E7`) using the real RocketNet wire protocol instead of synthetic ping packets.

#### E2: Single-Threaded Emscripten WASM Client Size, Memory Profile, and Appendix A KAT Gate
* **ID:** `E2` (Rank 2, Score `18.0`)
* **Topic:** `§4 Client library`
* **Question it answers:** What is the exact `.wasm` binary size (raw, gzipped, Brotli), linear memory footprint, and per-packet ChaCha20-Poly1305 throughput of the C++ client compiled with `-Oz -flto`, and does it pass 100% of `STANDARD.md` Appendix A Known-Answer Tests in Chrome, Firefox, and Safari?
* **Hypothesis:** With `-Oz -flto` and browser-sized receive ring constants (`ROCKETNET_NUM_CLIENT_PACKETS = 128`), the `.wasm` binary is $< 115\text{ KB}$ gzipped (`< 90 KB` Brotli), linear memory is $< 2.0\text{ MiB}$, per-packet encrypt/decrypt is $< 4\ \mu\text{s}$, and all Appendix A KAT vectors pass bit-for-bit.
* **What would be built:** A headless Playwright test runner executing `sdk/rocketnet_test_vectors.h` inside WASM across Chromium, Firefox, and WebKit, plus a size/memory/microbenchmark report.
* **Measurement and machine:** macOS / Linux CI bench; `wc -c`, `gzip -9`, `brotli -q 11`, `WebAssembly.Memory.prototype.buffer.byteLength`, and 100,000-iteration AEAD loop.
* **Cost in worker-days:** `0.5 worker-days`.
* **Dead-end criterion:** Gzipped `.wasm` $> 250\text{ KB}$ after dead-code elimination or Appendix A KAT failure.
* **What it unblocks:** Confirms whether Family A (Emscripten WASM) is small and fast enough to serve as the primary browser client or if Family B (TypeScript) is required immediately.

#### E3: Cross-Browser `WebTransport` + `serverCertificateHashes` Compatibility & `maxDatagramSize` Probe
* **ID:** `E3` (Rank 3, Score `18.0`)
* **Topic:** `§1 Transports`, `§6 Support matrix`
* **Question it answers:** Does a single WebTransport gateway using a 14-day self-signed ECDSA P-256 certificate (`serverCertificateHashes`) and the `wt.datagrams.writable ||= wt.datagrams.createWritable()` polyfill work identically across Chrome 157, Edge 154, Firefox 160, and Safari 26.4 (macOS & iOS 26.4), and what `maxDatagramSize` does each browser report before and after PMTUD?
* **Hypothesis:** All four browsers connect in 1 RTT using `serverCertificateHashes`; Safari 26.4 requires `createWritable()`; initial `maxDatagramSize` is between `1171 B` and `1200 B` on first frame before growing to $\ge 1350\text{ B}$ after PMTUD.
* **What would be built:** A standalone WebTransport test gateway (Go `quic-go`/`webtransport-go` or C `ngtcp2`) generating an ECDSA P-256 cert valid for 13 days, serving a test page that logs `wt.datagrams.maxDatagramSize` at $t = 0\text{ ms}, 50\text{ ms}, 500\text{ ms}$ and sends payload sizes from `1000 B` to `1400 B` in 8-byte steps.
* **Measurement and machine:** macOS machine running Chrome 157, Edge 154, Firefox 160, Safari 26.4 + iOS 26.4 simulator/device.
* **Cost in worker-days:** `0.5 worker-days`.
* **Dead-end criterion:** Safari 26.4 or Firefox 160 fails to connect with `serverCertificateHashes` or permanently caps `maxDatagramSize < 1150 B`.
* **What it unblocks:** Settles the exact `max_payload_bytes` clamp (`1120 B` vs `1192 B`) and certificate rotation automation for WebTransport.

#### E4: Gateway-Assisted Loopback Cookie Prefetch (3-RTT $\rightarrow$ 2-RTT Wide-Area Handshake)
* **ID:** `E4` (Rank 4, Score `16.0`)
* **Topic:** `§3 Protocol mapping`
* **Question it answers:** Can the gateway intercept a browser's initial `Type 0 Connection Request` (`710 B`, zero cookie), complete the `Type 7 Connection Cookie` (`38 B`) round-trip against the local native server over `127.0.0.1`, splice the 24-byte cookie into bytes `6..30` of the cached `Type 0` packet, and immediately obtain the `66 B` `Type 2 Connection Challenge` in a single browser RTT?
* **Hypothesis:** Because `STANDARD.md:1204-1225` leaves bytes `6..30` unauthenticated by the client (`the client has no session keys yet`), loopback cookie splicing works with **zero changes** to the native C++ server and cuts wide-area handshake latency by exactly `1.0 * RTT` (`50 ms` saved at `50 ms` RTT).
* **What would be built:** A 25-line state machine inside the gateway shim from `E1` tested over a `50 ms` `netem` delay link.
* **Measurement and machine:** Linux bench with `tc netem delay 25ms` (`50 ms` RTT); measure `t_connected - t_start` over 200 handshakes with and without loopback cookie splicing.
* **Cost in worker-days:** `0.5 worker-days`.
* **Dead-end criterion:** Native server rejects the spliced `Type 0` packet or time saved is $< 0.8 \times \text{RTT}$.
* **What it unblocks:** Gives WebTransport (`1 RTT` QUIC + `1.5 RTT` RocketNet = `2.5 RTTs` total) native-equivalent connection setup speed.

#### E5: [Overturn Test #1] Transport Latency & Jitter Under `netem` Impairment (WebTransport vs. WebRTC vs. WebSocket)
* **ID:** `E5` (Rank 5, Score `10.0` — **Can Overturn Our Primary Transport Recommendation**)
* **Topic:** `§1 Transports`, `§5 Measurement plan`
* **Question it answers:** At 10, 30, and 60 Hz with 100–1200 B payloads across `0%, 1%, 5%, 10%` random loss, Gilbert-Elliott burst loss, and `20, 50, 100 ms` RTT, does WebTransport (`QUIC DATAGRAM`) beat WebRTC `RTCDataChannel` (`ordered: false, maxRetransmits: 0`) on $p_{99}$ and $p_{99.9}$ one-way latency, or does QUIC's congestion controller stall outgoing datagrams under loss?
* **Hypothesis:** WebTransport matches native UDP within `3 ms` at $p_{99}$ under `1%` loss when `outgoingMaxAge = 50 ms` is set, and beats WebRTC on setup time by $> 3\times$ (`~125 ms` vs `~400 ms` at `50 ms` RTT).
* **What would be built:** Automated Playwright + `tc netem` harness running 72,000-packet (60s $\times$ 20 tabs) trials across Native UDP, WebTransport, WebRTC (`ordered: false, maxRetransmits: 0`), and single WebSocket.
* **Measurement and machine:** Linux bench (`netem` namespace bridge) + macOS/Linux browsers; `p50, p90, p99, p99.9` OWD and inter-arrival gap.
* **Cost in worker-days:** `1.0 worker-day`.
* **Dead-end / Overturn criterion:** **Overturns WebTransport as primary transport** if WebTransport $p_{99}$ OWD at `60 Hz, 50 ms RTT, 5% loss` exceeds WebRTC `RTCDataChannel` by $> 10\text{ ms}$ even with `outgoingMaxAge` configured.
* **What it unblocks:** Definitively locks in the primary transport (`WebTransport` vs. `WebRTC`).

#### E6: [Overturn Test #2] End-to-End RocketNet AEAD vs. Gateway-Terminated Encryption
* **ID:** `E6` (Rank 6, Score `8.0` — **Can Overturn Our End-to-End Encryption Recommendation**)
* **Topic:** `§3 Protocol mapping`
* **Question it answers:** What is the exact CPU, latency, and mobile power cost of keeping RocketNet's ChaCha20-Poly1305 AEAD end-to-end inside WebTransport/WebRTC vs. terminating encryption at the gateway and sending unencrypted payloads inside QUIC/DTLS?
* **Hypothesis:** End-to-end ChaCha20-Poly1305 adds $< 4\ \mu\text{s}$ per packet in browser WASM (`< 0.05%` of a 60 Hz frame) and **reduces** gateway CPU by ~25% compared to decrypting QUIC/DTLS and re-encrypting RocketNet in the gateway.
* **What would be built:** Benchmark harness comparing Mode A (browser runs full RocketNet AEAD; gateway relays raw ciphertext) vs. Mode B (browser sends plaintext over WebTransport; gateway seals RocketNet packets).
* **Measurement and machine:** Linux server bench (`perf stat` cycles/packet) + iPhone / Android phone (`performance.now()` + power profile over 15 minutes at 60 Hz).
* **Cost in worker-days:** `1.0 worker-day`.
* **Dead-end / Overturn criterion:** **Overturns End-to-End AEAD recommendation** if browser WASM ChaCha20-Poly1305 consumes $> 5\%$ of a 16.67 ms frame on a mid-range mobile phone or increases gateway CPU.
* **What it unblocks:** Finalizes whether `STANDARD.md` stays 100% untouched end-to-end.

#### E7: Striped Multi-WebSocket ($K=1, 2, 3, 4$) + `TCP_NOTSENT_LOWAT` Head-of-Line Blocking Mitigation
* **ID:** `E7` (Rank 7, Score `8.0`)
* **Topic:** `§1 Transports`, `§5 Measurement plan`, `§7 Ideas`
* **Question it answers:** By how many milliseconds does striping RocketNet packets across $K = 3$ parallel `wss://` connections (choosing the socket with `bufferedAmount === 0` + Linux `TCP_NOTSENT_LOWAT`) reduce $p_{99}$ and $p_{99.9}$ head-of-line delay compared to a single WebSocket under `1%` and `5%` packet loss?
* **Hypothesis:** At `60 Hz, 50 ms RTT, 2% loss`, a single WebSocket suffers $p_{99}$ OWD spikes of `+60–110 ms`, whereas $K=3$ striped WebSockets with `bufferedAmount` shedding cuts $p_{99}$ added delay by $\ge 60\%$ (`to < +25 ms`).
* **What would be built:** Multi-socket JS WebSocket driver + gateway session pool binding $K$ TCP connections to one `udp_fd[i]`.
* **Measurement and machine:** Linux `netem` bench (`0%, 1%, 2%, 5%` loss at `50 ms` RTT), 72,000 packets per $K \in \{1, 2, 3, 4\}$.
* **Cost in worker-days:** `1.0 worker-day`.
* **Dead-end criterion:** $K=3$ striping reduces $p_{99}$ OWD by $< 25\%$ compared to $K=1$, in which case fallback stays a single `wss://` socket.
* **What it unblocks:** Determines whether the WebSocket fallback can deliver playable 60 Hz action games behind strict UDP-blocking school/corporate firewalls.

#### E8: [Overturn Test #3] Co-Located Loopback UDP Gateway vs. In-Process Sans-I/O `ngtcp2` + Shared-Memory Driver
* **ID:** `E8` (Rank 8, Score `6.0` — **Can Overturn Our Sidecar Gateway Recommendation**)
* **Topic:** `§2 Server side`
* **Question it answers:** What is the latency, CPU, and PPS/core difference between (a) a separate gateway process forwarding over `127.0.0.1` UDP sockets, (b) a separate gateway process forwarding over a shared-memory ring (`rocketnet_shm_driver`), and (c) embedding sans-I/O `ngtcp2` / `libdatachannel` directly inside the native C++ server process?
* **Hypothesis:** Loopback UDP adds $< 25\ \mu\text{s}$ p50 and scales to $\ge 80{,}000\text{ pps/core}$, while a shared-memory ring driver matches in-process embedding within `5%` CPU while keeping OpenSSL/BoringSSL out of the core server process and preserving `AF_XDP` on the external NIC.
* **What would be built:** Three gateway-to-server coupling prototypes benchmarked at `1,000` clients $\times$ `60 Hz` (`120,000 pps`).
* **Measurement and machine:** Linux server bench (`perf stat`, `epoll` + `AF_XDP` coexistence test, p50/p99/p99.9 forwarding latency).
* **Cost in worker-days:** `1.5 worker-days`.
* **Dead-end / Overturn criterion:** **Overturns Sidecar Gateway recommendation** if inter-process forwarding (even via shared memory) adds $> 100\ \mu\text{s}$ p99 latency or costs $> 25\%$ more total CPU than in-process embedding.
* **What it unblocks:** Settles the production deployment boundary between `librocketnet` and the browser gateway.

#### E9: Pure TypeScript + `libsodium.js` Client vs. Emscripten C++ WASM Client
* **ID:** `E9` (Rank 9, Score `5.3`)
* **Topic:** `§4 Client library`
* **Question it answers:** Does a clean-room TypeScript implementation of `STANDARD.md` + `docs/CHANNELS.md` using `libsodium.js` (or minimal WASM crypto) achieve a $\ge 40\%$ smaller bundle and $\ge 80\%$ lower RAM footprint than the Emscripten C++ WASM build while passing 100% of Appendix A KAT vectors and `CHANNELS.md` reliability tests?
* **Hypothesis:** The TypeScript + slim WASM crypto client weighs `~35 KB` gzipped (vs `~105 KB` for full Emscripten C++) and uses `< 512 KiB` RAM per instance, while Emscripten C++ remains best for Unity WebGL / C# consumers.
* **What would be built:** A TypeScript prototype of the RocketNet client protocol (`Types 0–9`, replay window with the `F1` 64-word clear fix, fragment reassembly) tested against Appendix A KAT vectors.
* **Measurement and machine:** Linux/macOS bench; bundle size (`gzip`/`brotli`), heap snapshot in Chrome DevTools, and 60 Hz CPU profile.
* **Cost in worker-days:** `1.5 worker-days`.
* **Dead-end criterion:** TypeScript per-packet CPU cost exceeds `15 us/pkt` or bundle savings are $< 30\text{ KB}$ gzipped after including `CHANNELS.md`.
* **What it unblocks:** Decides whether to ship one WASM binary for all web/Unity targets or maintain a separate pure-TS package.

#### E10: Happy-Eyeballs Transport Cascade & Mid-Match Warm Upgrade (`WebSocket` $\rightarrow$ `WebTransport`/`WebRTC`)
* **ID:** `E10` (Rank 10, Score `5.3`)
* **Topic:** `§1 Transports`, `§7 Ideas`
* **Question it answers:** Can a browser client start a session immediately over `WebSocket` (or `WebTransport`), complete the RocketNet handshake, and seamlessly switch its active transport to `WebRTC` (or recover from a dropped transport) mid-session by rebinding the gateway's `udp_fd[i]` without dropping the native RocketNet connection?
* **Hypothesis:** Because RocketNet's connection identity on the native server is `udp_fd[i]`'s loopback port and RocketNet's AEAD authenticates every packet (`Packet Type 5` / `Type 8`), the gateway can verify a single AEAD-valid packet on the new transport and atomically swap the return path with zero lost reliable messages (`RELIABLE_ORDERED` in `CHANNELS.md`).
* **What would be built:** Client cascade controller + gateway atomic transport pointer swap.
* **Measurement and machine:** Linux `netem` bench simulating UDP/443 blackhole (`iptables -A INPUT -p udp --dport 443 -j DROP`) and mid-match transport upgrade; measure time-to-first-byte and lost ticks during handover.
* **Cost in worker-days:** `1.5 worker-days`.
* **Dead-end criterion:** Handover causes a sequence gap that stalls `RELIABLE_ORDERED` channels for $> 100\text{ ms}$.
* **What it unblocks:** Guarantees instant connection setup and automatic fallback across broken UDP networks.

#### E11: 2,500-Client Gateway Scale Shootout (`ngtcp2`/`quiche` vs. `libdatachannel`/`pion` vs. `uWebSockets`)
* **ID:** `E11` (Rank 11, Score `4.5`)
* **Topic:** `§2 Server side`, `§5 Measurement plan`
* **Question it answers:** How many CPU cores and megabytes of RAM does each gateway library (`ngtcp2`/`quiche` for WebTransport, `libdatachannel` vs. `pion/webrtc` for WebRTC ICE-Lite UDP-Mux, `uWebSockets` for WebSockets) consume when relaying `2,500` concurrent clients at `60 Hz` bidirectional (`300,000 pps` total)?
* **Hypothesis:** `ngtcp2`/`quiche` and `uWebSockets` handle `2,500` clients at 60 Hz in $\le 3$ cores and $< 120\text{ MiB}$ RSS, whereas `pion/webrtc` or `libdatachannel` requires $4\text{–}6$ cores and `200–350 MiB` RSS due to DTLS + `usrsctp`/SCTP state machines.
* **What would be built:** Multi-threaded load generator + gateway processes pinned to dedicated cores on Linux.
* **Measurement and machine:** Linux multi-core bench; `perf stat`, `/proc/pid/smaps_rollup`, p99 relay latency under 2,500-client load.
* **Cost in worker-days:** `2.0 worker-days`.
* **Dead-end criterion:** Any candidate library exceeds `250 KiB` RAM/client or `< 30,000 pps/core`.
* **What it unblocks:** Selects the exact production C/C++/Rust/Go libraries for the Browser Gateway.

#### E12: Mobile Cellular Handoff (`Wi-Fi` $\rightarrow$ `5G`), CGNAT, and `visibilitychange` Background Survival
* **ID:** `E12` (Rank 12, Score `4.7`)
* **Topic:** `§3 Protocol mapping`, `§6 Support matrix`, `§7 Ideas`
* **Question it answers:** When a player on an iPhone (iOS 26.4) or Android phone toggles Wi-Fi off (forcing a Wi-Fi $\rightarrow$ 5G IP change) or backgrounds the browser tab for 10 seconds (`visibilitychange`), does QUIC connection migration / WebRTC ICE restart / gateway loopback port persistence keep the native RocketNet session alive without a disconnect?
* **Hypothesis:** Because the gateway preserves the local `127.0.0.1:port_i` socket toward the native RocketNet server, QUIC connection migration (`RFC 9000 §9`) and fast WebSocket/WebRTC rebinding survive a Wi-Fi $\rightarrow$ 5G IP change with `< 500 ms` interruption, whereas a native UDP client would be dropped immediately by RocketNet's source-address identity rule!
* **What would be built:** Mobile test page with `visibilitychange` beacon + gateway 20-second keep-alive proxy and session rebind token.
* **Measurement and machine:** Physical iPhone (iOS 26.4) + Android phone switching between Wi-Fi and 5G cellular while connected to a cloud/LAN Linux server.
* **Cost in worker-days:** `1.5 worker-days`.
* **Dead-end criterion:** Handover takes $> 5.0\text{ s}$ or triggers a slot timeout on the native server.
* **What it unblocks:** Makes the browser client **more resilient to mobile network handoffs than native UDP**!

---

## 9 Sources

Every external specification, browser compatibility table, and source repository below was opened and read on **`2026-10-14`**:

1. `[S1]` W3C Candidate Recommendation, *WebTransport* (Editors: Bernard Aboba, Nidhi Jaju, Victor Vasiliev) — https://www.w3.org/TR/webtransport/ — Opened `2026-10-14`: Normative `WebTransport` API, `datagrams.createWritable()`, `maxDatagramSize`, and `§6.9 custom-certificate-requirements` ($\le 14$-day validity, ECDSA P-256 required, no RSA, `allowPooling: false`).
2. `[S2]` W3C Recommendation, *WebRTC 1.0: Real-Time Communication Between Browsers* (Editors: Cullen Jennings, Florent Castelli, Henrik Boström, Jan-Ivar Bruaroey) — https://www.w3.org/TR/webrtc/ — Opened `2026-10-14`: Normative `RTCPeerConnection` and `RTCDataChannel` specification (`ordered`, `maxRetransmits`, `maxPacketLifeTime`, `negotiated`).
3. `[S3]` WHATWG Living Standard, *WebSockets Standard* (Editor: Adam Rice) — https://websockets.spec.whatwg.org/ — Opened `2026-10-14`: Browser `WebSocket` API specification, `bufferedAmount`, `binaryType`, and closing handshake.
4. `[S4]` IETF RFC 9297, *HTTP Datagrams and the Capsule Protocol* (D. Schinazi, L. Pardue, August 2022) — https://datatracker.ietf.org/doc/html/rfc9297 — Opened `2026-10-14`: Quarter-Stream-ID framing for HTTP/3 Datagrams over QUIC `DATAGRAM` frames.
5. `[S5]` IETF RFC 9221, *An Unreliable Datagram Extension to QUIC* (T. Pauly, E. Kinnear, D. Schinazi, March 2022) — https://datatracker.ietf.org/doc/html/rfc9221 — Opened `2026-10-14`: QUIC `DATAGRAM` frame (`0x30`/`0x31`), MTU enforcement (no fragmentation), and congestion-control requirement (`§5.2`).
6. `[S6]` IETF RFC 8831, *WebRTC Data Channels* (R. Jesup, S. Loreto, M. Tüxen, January 2021) — https://datatracker.ietf.org/doc/html/rfc8831 — Opened `2026-10-14`: SCTP over DTLS architecture, PR-SCTP (`FORWARD-TSN`), unordered delivery (`U` bit), and congestion control.
7. `[S7]` IETF RFC 8832, *WebRTC Data Channel Establishment Protocol (DCEP)* (R. Jesup, S. Loreto, M. Tüxen, January 2021) — https://datatracker.ietf.org/doc/html/rfc8832 — Opened `2026-10-14`: In-band `DATA_CHANNEL_OPEN` / `DATA_CHANNEL_ACK` vs. pre-negotiated channels.
8. `[S8]` IETF RFC 6455, *The WebSocket Protocol* (I. Fette, A. Melnikov, December 2011) — https://datatracker.ietf.org/doc/html/rfc6455 — Opened `2026-10-14`: Wire framing (`2–14` bytes header + client 4-byte mask) and HTTP/1.1 Upgrade handshake.
9. `[S9]` Can I Use, *WebTransport Support Table* — https://caniuse.com/webtransport — Opened `2026-10-14`: Browser version support for WebTransport (`Chrome 97+`, `Edge 98+`, `Firefox 114+`, `Safari 26.4+`, `iOS Safari 26.4+`, Baseline Newly Available `2026-03-24`).
10. `[S10]` Can I Use, *WebRTC Peer-to-peer connections (`RTCPeerConnection`)* — https://caniuse.com/rtcpeerconnection — Opened `2026-10-14`: Browser support table for WebRTC (`Chrome 23+`, `Edge 79+`, `Firefox 22+`, `Safari 11+`, `iOS Safari 11.0+`, Baseline High `2022-07-15`).
11. `[S11]` Can I Use, *Web Sockets Support Table* — https://caniuse.com/websockets — Opened `2026-10-14`: Browser support table for WebSockets (`Chrome 16+`, `Edge 12+`, `Firefox 11+`, `Safari 7+`, `iOS Safari 6.0+`, Baseline High `2018-01-29`).
12. `[S12]` Can I Use, *SharedArrayBuffer Support Table* — https://caniuse.com/sharedarraybuffer — Opened `2026-10-14`: Browser support for `SharedArrayBuffer` (`Chrome 68+`, `Edge 79+`, `Firefox 79+`, `Safari 15.2+`, `iOS Safari 15.2+`; unsupported in Android WebView).
13. `[S13]` Can I Use, *WebAssembly (WASM) Support Table* — https://caniuse.com/wasm — Opened `2026-10-14`: Browser support for WebAssembly (`Chrome 57+`, `Edge 16+`, `Firefox 52+`, `Safari 11+`, `iOS Safari 11+`).
14. `[S14]` Can I Use, *Web Cryptography API Support Table* — https://caniuse.com/cryptography — Opened `2026-10-14`: Browser support for `window.crypto.subtle`.
15. `[S15]` MDN Web Docs, *WebTransport Interface* — https://developer.mozilla.org/en-US/docs/Web/API/WebTransport — Opened `2026-10-14`: Overview of `WebTransport` properties, methods, and worker availability.
16. `[S16]` MDN Web Docs, *WebTransport() Constructor and Options* — https://developer.mozilla.org/en-US/docs/Web/API/WebTransport/WebTransport — Opened `2026-10-14`: Constructor options `allowPooling`, `congestionControl`, `protocols`, `requireUnreliable`, and `serverCertificateHashes`.
17. `[S17]` MDN Web Docs, *WebTransportDatagramDuplexStream* — https://developer.mozilla.org/en-US/docs/Web/API/WebTransportDatagramDuplexStream — Opened `2026-10-14`: Datagram duplex stream interface (`maxDatagramSize`, `incomingMaxAge`, `outgoingMaxAge`, `readable`, `writable`, `createWritable`).
18. `[S18]` MDN Web Docs, *RTCDataChannel* — https://developer.mozilla.org/en-US/docs/Web/API/RTCDataChannel — Opened `2026-10-14`: Properties and methods of `RTCDataChannel` (`ordered`, `maxPacketLifeTime`, `maxRetransmits`, `bufferedAmount`, `binaryType`).
19. `[S19]` MDN Web Docs, *RTCPeerConnection.createDataChannel()* — https://developer.mozilla.org/en-US/docs/Web/API/RTCPeerConnection/createDataChannel — Opened `2026-10-14`: Options dictionary for creating unreliable/unordered and pre-negotiated WebRTC data channels.
20. `[S20]` MDN Web Docs, *SubtleCrypto* — https://developer.mozilla.org/en-US/docs/Web/API/SubtleCrypto — Opened `2026-10-14`: Supported Web Crypto API methods and algorithms.
21. `[S21]` MDN Web Docs, *SubtleCrypto.encrypt()* — https://developer.mozilla.org/en-US/docs/Web/API/SubtleCrypto/encrypt — Opened `2026-10-14`: Supported encryption algorithms in WebCrypto (`AES-CTR`, `AES-CBC`, `AES-GCM`, `RSA-OAEP`; no `ChaCha20-Poly1305`).
22. `[S22]` MDN Web Docs, *SubtleCrypto.sign()* — https://developer.mozilla.org/en-US/docs/Web/API/SubtleCrypto/sign — Opened `2026-10-14`: Signature algorithms in WebCrypto including `Ed25519`.
23. `[S23]` MDN Web Docs, *SubtleCrypto.deriveBits()* — https://developer.mozilla.org/en-US/docs/Web/API/SubtleCrypto/deriveBits — Opened `2026-10-14`: Key derivation algorithms in WebCrypto including `X25519`, `ECDH`, `HKDF`, and `PBKDF2`.
24. `[S24]` MDN Web Docs, *WebSocket* — https://developer.mozilla.org/en-US/docs/Web/API/WebSocket — Opened `2026-10-14`: `WebSocket` interface, `bufferedAmount`, and worker support.
25. `[S25]` MDN Web Docs, *AudioWorklet* — https://developer.mozilla.org/en-US/docs/Web/API/AudioWorklet — Opened `2026-10-14`: Audio processing thread scope and its lack of networking APIs.
26. `[S26]` MDN Web Docs, *OffscreenCanvas* — https://developer.mozilla.org/en-US/docs/Web/API/OffscreenCanvas — Opened `2026-10-14`: Rendering WebGL/WebGPU off the main thread inside a Web Worker.
27. `[S27]` MDN Web Docs, *Window.crossOriginIsolated* — https://developer.mozilla.org/en-US/docs/Web/API/Window/crossOriginIsolated — Opened `2026-10-14`: Cross-origin isolation requirements for `SharedArrayBuffer` and unthrottled `performance.now()` timers.
28. `[S28]` MDN Web Docs, *Cross-Origin-Opener-Policy (COOP)* — https://developer.mozilla.org/en-US/docs/Web/HTTP/Reference/Headers/Cross-Origin-Opener-Policy — Opened `2026-10-14`: HTTP header specification and popup/iframe isolation effects.
29. `[S29]` MDN Web Docs, *Cross-Origin-Embedder-Policy (COEP)* — https://developer.mozilla.org/en-US/docs/Web/HTTP/Reference/Headers/Cross-Origin-Embedder-Policy — Opened `2026-10-14`: `require-corp` and `credentialless` headers required for `SharedArrayBuffer`.
30. `[S30]` MDN Web Docs, *SharedArrayBuffer* — https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/SharedArrayBuffer — Opened `2026-10-14`: Shared WebAssembly memory requirements for Emscripten pthreads.
31. `[S31]` MDN Web Docs, *Performance.now()* — https://developer.mozilla.org/en-US/docs/Web/API/Performance/now — Opened `2026-10-14`: Timer resolution (`5 us` when cross-origin isolated vs. `100 us` default).
32. `[S32]` MDN Web Docs, *WebGPU API* — https://developer.mozilla.org/en-US/docs/Web/API/WebGPU_API — Opened `2026-10-14`: Modern browser GPU compute and rendering API.
33. `[S33]` IETF Internet-Draft, *WebTransport over HTTP/3* (`draft-ietf-webtrans-http3`, Alan Frindell, Eric Kinnear, Victor Vasiliev) — https://datatracker.ietf.org/doc/html/draft-ietf-webtrans-http3/ — Opened `2026-10-14`: Extended CONNECT setup, session multiplexing, and capsule/datagram association.
34. `[S34]` IETF Internet-Draft, *Media over QUIC Transport* (`draft-ietf-moq-transport`, Luke Curley, Kirill Pugin, Suhas Nandakumar, Victor Vasiliev) — https://datatracker.ietf.org/doc/html/draft-ietf-moq-transport/ — Opened `2026-10-14`: Pub/sub media protocol layered over WebTransport/QUIC.
35. `[S35]` IETF RFC 9000, *QUIC: A UDP-Based Multiplexed and Secure Transport* (J. Iyengar, M. Thomson, May 2021) — https://datatracker.ietf.org/doc/html/rfc9000 — Opened `2026-10-14`: 1200-byte minimum initial datagram size (`§14`), connection migration (`§9`), and 1-RTT/0-RTT handshake.
36. `[S36]` IETF RFC 9002, *QUIC Loss Detection and Congestion Control* (J. Iyengar, I. Swett, May 2021) — https://datatracker.ietf.org/doc/html/rfc9002 — Opened `2026-10-14`: QUIC congestion window, pacing, and PTO mechanics.
37. `[S37]` IETF RFC 9114, *HTTP/3* (M. Bishop, June 2022) — https://datatracker.ietf.org/doc/html/rfc9114 — Opened `2026-10-14`: HTTP/3 framing over QUIC.
38. `[S38]` IETF RFC 8445, *Interactive Connectivity Establishment (ICE)* (A. Keranen, C. Holmberg, J. Rosenberg, July 2018) — https://datatracker.ietf.org/doc/html/rfc8445 — Opened `2026-10-14`: Full ICE vs. ICE-Lite (`§2.2`) connectivity checks.
39. `[S39]` IETF RFC 8489, *Session Traversal Utilities for NAT (STUN)* (M. Petit-Huguenin et al., February 2020) — https://datatracker.ietf.org/doc/html/rfc8489 — Opened `2026-10-14`: STUN Binding Request/Response framing (`20 B` header).
40. `[S40]` IETF RFC 8656, *Traversal Using Relays around NAT (TURN)* (T. Reddy et al., February 2020) — https://datatracker.ietf.org/doc/html/rfc8656 — Opened `2026-10-14`: TURN Allocate, ChannelData (`4 B` framing), and UDP/TCP/TLS relay transport.
41. `[S41]` IETF RFC 8829, *JavaScript Session Establishment Protocol (JSEP)* (J. Uberti, C. Jennings, E. Rescorla, January 2021) — https://datatracker.ietf.org/doc/html/rfc8829 — Opened `2026-10-14`: SDP offer/answer state machine and `a=fingerprint` self-signed DTLS certificate verification.
42. `[S42]` IETF RFC 8260, *Stream Schedulers and User Message Interleaving for the Stream Control Transmission Protocol (SCTP)* (R. Stewart et al., November 2017) — https://datatracker.ietf.org/doc/html/rfc8260 — Opened `2026-10-14`: `I-DATA` chunk interleaving to prevent large SCTP messages from monopolizing an association.
43. `[S43]` IETF RFC 9260, *Stream Control Transmission Protocol (SCTP)* (R. Stewart, M. Tüxen, K. Nielsen, June 2022) — https://datatracker.ietf.org/doc/html/rfc9260 — Opened `2026-10-14`: SCTP 4-way handshake (`INIT`, `INIT-ACK`, `COOKIE-ECHO`, `COOKIE-ACK`), `DATA`/`SACK` chunks, and congestion control.
44. `[S44]` IETF RFC 6347, *Datagram Transport Layer Security Version 1.2* (E. Rescorla, N. Modadugu, January 2012) — https://datatracker.ietf.org/doc/html/rfc6347 — Opened `2026-10-14`: DTLS 1.2 record layer (`13 B` header) and 2-RTT handshake.
45. `[S45]` IETF RFC 9147, *The Datagram Transport Layer Security (DTLS) Protocol Version 1.3* (E. Rescorla, H. Tschofenig, N. Modadugu, April 2022) — https://datatracker.ietf.org/doc/html/rfc9147 — Opened `2026-10-14`: DTLS 1.3 1-RTT handshake and compact record header.
46. `[S46]` IETF RFC 7539, *ChaCha20 and Poly1305 for IETF Protocols* (Y. Nir, A. Langley, May 2015) — https://datatracker.ietf.org/doc/html/rfc7539 — Opened `2026-10-14`: 96-bit nonce + 32-bit counter AEAD construction used by RocketNet.
47. `[S47]` IETF RFC 7693, *The BLAKE2 Cryptographic Hash and Message Authentication Code (MAC)* (M-J. Saarinen, J-P. Aumasson, November 2015) — https://datatracker.ietf.org/doc/html/rfc7693 — Opened `2026-10-14`: BLAKE2b hash and keyed MAC used by RocketNet key derivation and cookies.
48. `[S48]` IETF RFC 8032, *Edwards-Curve Digital Signature Algorithm (EdDSA)* (S. Josefsson, I. Liusvaara, January 2017) — https://datatracker.ietf.org/doc/html/rfc8032 — Opened `2026-10-14`: Ed25519 signature verification used on RocketNet Connect Tokens.
49. `[S49]` IETF RFC 7748, *Elliptic Curves for Security* (A. Langley, M. Hamburg, S. Turner, January 2016) — https://datatracker.ietf.org/doc/html/rfc7748 — Opened `2026-10-14`: X25519 Diffie-Hellman key agreement used in RocketNet handshake and sealed boxes.
50. `[S50]` IETF RFC 8305, *Happy Eyeballs Version 2: Better Connectivity Using Concurrency* (D. Schinazi, T. Pauly, December 2017) — https://datatracker.ietf.org/doc/html/rfc8305 — Opened `2026-10-14`: Staggered connection racing timer design (`150–250 ms`).
51. `[S51]` IETF RFC 8888, *RTP Control Protocol (RTCP) Feedback for Congestion Control* (Z. Sarker et al., January 2021) — https://datatracker.ietf.org/doc/html/rfc8888 — Opened `2026-10-14`: Congestion control feedback intervals for real-time traffic.
52. `[S52]` IETF RFC 8555, *Automatic Certificate Management Environment (ACME)* (R. Barnes et al., March 2019) — https://datatracker.ietf.org/doc/html/rfc8555 — Opened `2026-10-14`: Automated DNS-01 / HTTP-01 TLS certificate issuance for game servers.
53. `[S53]` W3C First Public Working Draft, *Web Cryptography Level 2* (Editor: Daniel Huigens, 22 April 2025) — https://www.w3.org/TR/webcrypto-2/ — Opened `2026-10-14`: Confirms `Ed25519` (`§25`) and `X25519` (`§26`) are in WebCrypto Level 2, while `ChaCha20-Poly1305`, `BLAKE2b`, and `SipHash` are absent (`0` occurrences).
54. `[S54]` MDN Browser Compat Data, *`api/WebTransport.json`* — https://raw.githubusercontent.com/mdn/browser-compat-data/main/api/WebTransport.json — Opened `2026-10-14`: Exact browser versions for `WebTransport` (`Chrome 97`, `Firefox 114`, `Safari 26.4`) and `serverCertificateHashes` (`Chrome 100`, `Firefox 125`, `Safari 26.4`).
55. `[S55]` MDN Browser Compat Data, *`api/WebTransportDatagramDuplexStream.json`* — https://raw.githubusercontent.com/mdn/browser-compat-data/main/api/WebTransportDatagramDuplexStream.json — Opened `2026-10-14`: Documents that Safari 26.4 supports `createWritable()` (`26.4`) and sets `writable` to `false`, whereas Chrome supports `writable` (`97`) and sets `createWritable()` to `false`.
56. `[S56]` MDN Browser Compat Data, *`api/RTCDataChannel.json`* — https://raw.githubusercontent.com/mdn/browser-compat-data/main/api/RTCDataChannel.json — Opened `2026-10-14`: Exact browser versions for `RTCDataChannel`, `ordered`, `maxRetransmits`, `maxPacketLifeTime`, and Worker `transferable` (`Chrome 130`, `Firefox 144`, `Safari 15`).
57. `[S57]` MDN Browser Compat Data, *`api/SubtleCrypto.json`* — https://raw.githubusercontent.com/mdn/browser-compat-data/main/api/SubtleCrypto.json — Opened `2026-10-14`: Exact browser versions for `Ed25519` (`Chrome 137`, `Firefox 129`, `Safari 17`) and `X25519` (`Chrome 133`, `Firefox 130`, `Safari 17`).
58. `[S58]` MDN Browser Compat Data, *`api/WebSocket.json`* — https://raw.githubusercontent.com/mdn/browser-compat-data/main/api/WebSocket.json — Opened `2026-10-14`: Exact browser versions for `WebSocket` and Worker support.
59. `[S59]` MDN Browser Compat Data, *`javascript/builtins/SharedArrayBuffer.json`* — https://raw.githubusercontent.com/mdn/browser-compat-data/main/javascript/builtins/SharedArrayBuffer.json — Opened `2026-10-14`: Confirms `SharedArrayBuffer` is unsupported in `webview_android` (`false`).
60. `[S60]` Can I Use Raw Dataset, *`features-json/webtransport.json`* (Fyrd/caniuse) — https://raw.githubusercontent.com/Fyrd/caniuse/main/features-json/webtransport.json — Opened `2026-10-14`: Version-by-version support matrix for `webtransport` through Chrome 157, Firefox 160, Safari 26.4–27.2.
61. `[S61]` Can I Use Raw Dataset, *`features-json/rtcpeerconnection.json`* (Fyrd/caniuse) — https://raw.githubusercontent.com/Fyrd/caniuse/main/features-json/rtcpeerconnection.json — Opened `2026-10-14`: Version-by-version support matrix for `rtcpeerconnection`.
62. `[S62]` Can I Use Raw Dataset, *`features-json/websockets.json`* (Fyrd/caniuse) — https://raw.githubusercontent.com/Fyrd/caniuse/main/features-json/websockets.json — Opened `2026-10-14`: Version-by-version support matrix for `websockets`.
63. `[S63]` GitHub Repository, *`paullouisageneau/libdatachannel`* (Paul-Louis Ageneau) — https://github.com/paullouisageneau/libdatachannel — Opened `2026-10-14`: C++17 WebRTC Data Channels & WebSockets library; MPL-2.0 license; last push `2026-10-07`; 2,760 stars.
64. `[S64]` GitHub Repository, *`webrtc-rs/webrtc`* (WebRTC.rs Project) — https://github.com/webrtc-rs/webrtc — Opened `2026-10-14`: Rust WebRTC stack; Apache-2.0 license; last push `2026-10-11`; 5,159 stars.
65. `[S65]` GitHub Repository, *`pion/webrtc`* (Sean DuBois & Pion Contributors) — https://github.com/pion/webrtc — Opened `2026-10-14`: Pure Go WebRTC implementation; MIT license; last push `2026-10-14`; 16,832 stars.
66. `[S66]` GitHub Repository, *`microsoft/msquic`* (Microsoft) — https://github.com/microsoft/msquic — Opened `2026-10-14`: C IETF QUIC implementation; MIT license; last push `2026-10-13`; 4,785 stars.
67. `[S67]` GitHub Repository, *`cloudflare/quiche`* (Cloudflare) — https://github.com/cloudflare/quiche — Opened `2026-10-14`: Rust/C sans-I/O QUIC and HTTP/3 implementation; BSD-2-Clause license; last push `2026-10-13`; 12,782 stars.
68. `[S68]` GitHub Repository, *`ngtcp2/ngtcp2`* (Tatsuhiro Tsujikawa) — https://github.com/ngtcp2/ngtcp2 — Opened `2026-10-14`: C sans-I/O IETF QUIC implementation; MIT license; last push `2026-10-11`; 1,528 stars.
69. `[S69]` GitHub Repository, *`litespeedtech/lsquic`* (LiteSpeed Technologies) — https://github.com/litespeedtech/lsquic — Opened `2026-10-14`: C QUIC and HTTP/3 implementation; MIT license; last push `2026-10-12`; 1,878 stars.
70. `[S70]` GitHub Repository, *`aws/s2n-quic`* (Amazon Web Services) — https://github.com/aws/s2n-quic — Opened `2026-10-14`: Rust IETF QUIC implementation; Apache-2.0 license; last push `2026-10-14`; 1,376 stars.
71. `[S71]` GitHub Repository, *`uNetworking/uWebSockets`* (Alex Hultman) — https://github.com/uNetworking/uWebSockets — Opened `2026-10-14`: C++17 / C WebSocket and HTTP server; Apache-2.0 license; last push `2026-10-05`; 18,999 stars.
72. `[S72]` GitHub Repository, *`jedisct1/libsodium.js`* (Frank Denis) — https://github.com/jedisct1/libsodium.js — Opened `2026-10-14`: libsodium compiled to WebAssembly and JS wrappers; ISC license; last push `2026-09-25`; 1,160 stars.
73. `[S73]` GitHub Repository, *`jedisct1/libsodium`* (Frank Denis) — https://github.com/jedisct1/libsodium — Opened `2026-10-14`: Upstream C cryptographic library vendored in `sodium/`; ISC license; last push `2026-10-13`; 13,987 stars.
74. `[S74]` GitHub Repository, *`emscripten-core/emscripten`* (Emscripten Contributors) — https://github.com/emscripten-core/emscripten — Opened `2026-10-14`: C/C++ to WebAssembly compiler toolchain; MIT / UIUC NCSA dual license; last push `2026-10-14`; 27,709 stars.
75. `[S75]` Google Git Repository, *`webrtc.googlesource.com/src` (`libwebrtc`)* (WebRTC Project) — https://webrtc.googlesource.com/src — Opened `2026-10-14`: Reference C++ WebRTC engine used in Chromium/Firefox/Safari; BSD-3-Clause license.

---

## 10 Not done

1. **No code built, compiled, or benchmarked (`By Card Scope`):** Per the strict `SCOPE` and `RULES` of this card, zero code was written, no `emcc` or `cmake` builds were executed, no servers were started, and no `netem` benchmarks were run. Consequently, all latency (`us`/`ms`), throughput (`pps/core`), and compressed WASM byte sizes in `## 1`–`## 4` are explicitly marked `[ESTIMATED]` with their underlying arithmetic shown, awaiting execution of `E1`–`E12`.
2. **Exact StatCounter Global Player Share Joins (`Marked [UNVERIFIED]` in `§6.2`):** While the exact browser version numbers where `WebTransport`, `RTCDataChannel`, `WebSocket`, `SharedArrayBuffer`, and `SubtleCrypto` (`Ed25519`/`X25519`) shipped were verified directly from the live `Fyrd/caniuse` and `mdn/browser-compat-data` JSON datasets on `2026-10-14` (`[S54–S62]`), the raw `Fyrd/caniuse` feature JSON files store `usage_perc_y: null` (computing global share dynamically in the browser UI from external StatCounter datasets), and empirical enterprise/campus firewall UDP drop percentages vary by geography and audience. Those share figures are therefore marked `[UNVERIFIED]` in `§6.2`.
3. **What I Would Do Next (Monday Morning Execution Order):**
   * **Step 1 (`E1` + `E2`, Day 1):** Execute `E1` (the single-threaded Emscripten WASM build of `sdk/rocketnet_client_transport.cpp` + `wasm_ring_driver` talking through a local gateway shim to the unmodified native C++ server on macOS) and run the `STANDARD.md` Appendix A KAT vectors (`E2`) in headless Chrome, Firefox, and Safari 26.4.
   * **Step 2 (`E3` + `E4`, Day 2):** Verify `WebTransport` + `serverCertificateHashes` + `wt.datagrams.writable ||= wt.datagrams.createWritable()` across Chrome 157, Firefox 160, and Safari 26.4 (`E3`), and wire the 25-line loopback cookie splicer (`E4`) to cut 1 full WAN RTT from the browser handshake.
   * **Step 3 (`E5` + `E7`, Days 3–4):** Run the automated `tc netem` latency/jitter benchmark (`E5`) comparing WebTransport, WebRTC (`ordered: false, maxRetransmits: 0`), single WebSocket, and $K=3$ Striped WebSockets (`E7`) at 10/30/60 Hz across 0/1/5/10% loss to lock in the primary and fallback transports with hard $p_{99}$ and $p_{99.9}$ numbers.
