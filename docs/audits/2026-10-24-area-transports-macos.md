# macOS portable and msgx transports audit, 2026-10-24

Cold reading audit of `mas-bandwidth/rocketnet` at `9775266a705148969cc00fcadc1463b06879cfbd`
(`main`). Scope read, top to bottom: the macOS and iOS section of
`sdk/rocketnet_platform.cpp` (platform init, time, the portable BSD-socket path,
the `sendmsg_x`/`recvmsg_x` batch pump with its fallback and pause window, the
threads, mutexes and the kqueue wake) and its shared tail (the pump-request
create, the portable seam fallback, the allocation sizes); `sdk/rocketnet_platform.h`;
the portable and msgx server and client operations in
`sdk/rocketnet_driver_builtin.cpp` and the registry rows in
`sdk/rocketnet_drivers.inc`; the msgx selection and memory plan in
`sdk/rocketnet_server_transport.cpp` and `sdk/rocketnet_client_transport.cpp`;
the transport availability table in `sdk/rocketnet_config.h`; `docs/MACOS.md`;
and the tests `tests/test_msgx.cpp` and `tests/test_unit_client_msgx.cpp`, with
`standard/TRANSPORT.md`, `TRANSPORTS.md` ("ICMP errors") and `tla/COVERAGE.tsv`
open beside them. Method: each candidate was traced from a reachable input or
call sequence to the line that misbehaves; a candidate with no reachable path is
graded risk or quality. Nothing was run on macOS: a finding that only a macOS
run could confirm says so. No code was changed. Findings are ordered most
severe first; each gives severity (`bug` / `risk` / `quality`), `file:line` at
the sha above, a one-sentence defect, a one-sentence concrete failure or cost,
and the smallest fix.

## Findings

### 1. bug — `sdk/rocketnet_platform.cpp:2577`

- Defect: `rocketnet_platform_id` in the section shared by macOS and iOS returns `ROCKETNET_PLATFORM_MAC` unconditionally, while `sdk/rocketnet_platform.h:76` documents it as the compile-time platform identifier and an iOS build compiles with `ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_IOS` (`sdk/rocketnet_internal.h`, `TARGET_OS_IPHONE`).
- Failure or cost: Every iOS process logs `platform is mac` at `rocketnet_init` (`sdk/rocketnet.cpp:734`), and any caller that branches on `rocketnet_platform_id()` treats an iOS device as a Mac, where the msgx transport and the macOS connection-type lookup exist and iOS has neither.
- Smallest fix: `return ROCKETNET_PLATFORM;` as the Linux and BSD section does at `sdk/rocketnet_platform.cpp:608`.

### 2. risk — `sdk/rocketnet_platform.cpp:2427`

- Defect: The macOS connection-type lookup in `rocketnet_platform_init` passes `primary_interface_name` (an unchecked `CFDictionaryGetValue` result, line 2420) and `bsd_name` (`SCNetworkInterfaceGetBSDName`, documented to return NULL for an interface with no BSD name) straight to `CFStringCompare`, which does not accept NULL.
- Failure or cost: On a Mac whose global IPv4 state lacks a `PrimaryInterface` key, or whose interface list holds an interface without a BSD name ahead of the primary one, `rocketnet_init` crashes inside CoreFoundation before any transport exists (unconfirmed by run).
- Smallest fix: Skip the loop when `primary_interface_name` is NULL or not a `CFString`, and `continue` past any interface whose `bsd_name` is NULL, leaving the connection type `UNKNOWN`.

### 3. risk — `sdk/rocketnet_platform.cpp:3591`

- Defect: Both receive arms retry immediately and without bound on any weather errno (`ENOBUFS`, `ENETDOWN`, `EHOSTDOWN`, `EINTR` and the ICMP class): the msgx arm `continue`s its chunk loop here and the portable arm `continue`s at line 3149, relying on the comment's claim (lines 3054–3057) that each weather report is one-shot.
- Failure or cost: If the kernel returns the same weather errno on consecutive calls (an mbuf shortage that persists for `ENOBUFS`, for example), the receive worker or the client I/O thread spins at full CPU inside one drain and never returns to the loop that reads `quit`, so a server stop or client destroy hangs; the send side refuses exactly this retry for `EINTR` (lines 3466–3468) for the same reason (unconfirmed by run).
- Smallest fix: Bound the weather skips per call (for example, at most `max_packets` skips, or end the drain after a weather errno that repeats without a datagram in between), so a drain always returns.

### 4. risk — `sdk/rocketnet_platform.cpp:3539`

- Defect: `rocketnet_platform_socket_receive_packets` puts a 65535-byte `overflow` array on the stack even when it goes straight to its per-packet path, and that path calls `rocketnet_platform_socket_receive_packet` (line 3643), which puts a second 65535-byte `overflow` array on the stack (line 3109).
- Failure or cost: Every batch receive on macOS and iOS with a paused or forced-portable pump needs about 130 KiB of stack, and with `ROCKETNET_CLIENT_STEP_DRIVEN=1` (`docs/MACOS.md`) that receive runs on the game's own thread, so a game that updates the client from a fiber or job-system thread with a 64 KiB or 128 KiB stack overflows it (unconfirmed by run).
- Smallest fix: Give the per-packet receive an internal form that takes the caller's overflow buffer and call that from `receive_packets`, so one 64 KiB array serves both paths.

### 5. risk — `sdk/rocketnet_platform.cpp:3508`

- Defect: After a refused `sendmsg_x` the whole unsent tail from `offset` is replayed through `rocketnet_platform_socket_send_packet`, which counts every refused send in `send_failures` (line 3018), although the comment above (lines 3466–3484) records that xnu can deliver a prefix of the batch and still return -1.
- Failure or cost: When a chunk is partly delivered and refused for a full send buffer (`EAGAIN` or `ENOBUFS`, both weather), the delivered prefix is resent into the same full buffer, refused again and counted, so the server and client `send_failures` statistics report as dropped datagrams that reached the wire (unconfirmed by run).
- Smallest fix: Name the overcount in the comment and the statistics documentation, or on a weather errno replay only what the per-packet path can attribute (count a replay refusal separately from a first-attempt refusal).

### 6. quality — `docs/MACOS.md:87`

- Defect: The page says the `portable` server is ranked 30 "only in development builds and unavailable in production" and that server `auto` "falls to `portable` only in a development build", but the registry row ranks it `(ROCKETNET_DEV ? 30 : 50)` (`sdk/rocketnet_drivers.inc:7`), which is available, and `sdk/rocketnet_config.h:248` lists `msgx, portable` for the macOS production server.
- Failure or cost: An operator reading the macOS page believes a production server whose msgx create refuses fails to start, when it in fact falls to the portable transport, so the documented failure mode and the shipped one differ.
- Smallest fix: Say that production ranks portable 50, behind msgx, and that a production server falls to it when msgx refuses.

### 7. quality — `docs/MACOS.md:111`

- Defect: The page says that on the first batch receive "a result of zero or more, or `EAGAIN`/`EWOULDBLOCK`, keeps the pump; any other errno pauses it", and its send weather list (line 120) omits `EINTR`, while the code keeps the pump on the receive weather errnos too (`rocketnet_platform_receive_weather`, line 3591) and classes `EINTR` as send weather (`rocketnet_msgx_send_weather`, line 3320).
- Failure or cost: A reader or test author working from the page expects an `ECONNREFUSED` on a client's first batch receive to pause msgx for 60 seconds, which it never does, and cannot tell from the page that an interrupted batch keeps the pump.
- Smallest fix: Name the receive weather set (or point to `TRANSPORTS.md` "ICMP errors") in that sentence and add `EINTR` to the send list.

### 8. quality — `sdk/rocketnet_platform.cpp:3264`

- Defect: The msgx pause and re-probe machine (`msgx_down_until` written by `rocketnet_msgx_pause` and cleared by a compare-and-swap in `rocketnet_msgx_active`, with the server's TX and RX workers on one socket racing it) has no module under `tla/` and no row in `tla/COVERAGE.tsv`; its only check is one hand-built race case (`rocketnet_msgx_test_pause_expiry_race`).
- Failure or cost: The rule its comment states ("a fresh pause published by the other worker wins") and the no-loss rule of the fallback rest on that single interleaving, so a later change to either worker's ordering has no model to fail against.
- Smallest fix: Add a small module (two workers, pause, expire, re-probe; invariant that a pause set after an expiry is never cleared by that expiry) and a `COVERAGE.tsv` row citing it from line 3264.

### 9. quality — `sdk/rocketnet_platform.cpp:3780`

- Defect: The macOS `rocketnet_platform_server_thread_priority` carries an "IMPORTANT: ... adjust the affinity and priority for the server thread here" comment, but no macOS server transport calls it (only the gso, iocp and rio transports do), and `TRANSPORTS.md:282` cites it at `sdk/rocketnet_platform.cpp:1434`, `:3142` and `:3573`, lines that now hold an IPv6 conversion loop and two receive branches.
- Failure or cost: A developer who follows the comment and tunes the macOS server thread priority changes nothing, because the portable and msgx workers never run this function, and the cites send a reader to unrelated code.
- Smallest fix: Say in the comment that no macOS server transport calls it, and replace the line cites with the function name.

### 10. quality — `sdk/rocketnet_platform.cpp:3152`

- Defect: The `error != EINTR` test in the portable receive can never be false, because `EINTR` is in `rocketnet_platform_receive_weather` and was already skipped by the `continue` at line 3149.
- Failure or cost: The dead clause tells a reader that `EINTR` ends a drain silently, which contradicts the weather rule two lines above and `TRANSPORTS.md` "ICMP errors".
- Smallest fix: Drop `&& error != EINTR` from the condition.

### 11. quality — `sdk/rocketnet_client_transport.cpp:955`

- Defect: The client memory plan accepts the name `msgx` on iOS as well as macOS, while the registry offers msgx only on macOS (`sdk/rocketnet_drivers.inc`, `ROCKETNET_TRANSPORT_MAC`), the server plan accepts it only on macOS (`sdk/rocketnet_driver_builtin.cpp:1209`), and `rocketnet_platform_socket_create_with_pump` refuses `msgx` on iOS.
- Failure or cost: On iOS `rocketnet_client_transport_required_memory( config, "msgx" )` answers a positive byte count for a transport that can never be created there, so the two endpoints' plans disagree about which names exist.
- Smallest fix: Guard the `msgx` name with `ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC` only, as the server plan does.

## Counts

- bug: 1 (finding 1)
- risk: 4 (findings 2–5)
- quality: 6 (findings 6–11)
- total: 11

## Already filed

None of the findings in `docs/audits/2026-10-24-code-quality-a.md` or
`docs/audits/2026-10-24-code-quality-b.md` falls in this area, so none is
repeated here.
