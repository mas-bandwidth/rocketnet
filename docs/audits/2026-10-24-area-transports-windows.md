# Windows portable, IOCP and RIO transports audit, 2026-10-24

Cold reading audit of `mas-bandwidth/rocketnet` at `9775266a705148969cc00fcadc1463b06879cfbd`
(`main`). Scope read, top to bottom: the Windows section of `sdk/rocketnet_platform.cpp`
(from line 3951) and the shared sections after it (socket create with a pump, the portable
batch pump, the wake fallback, the allocation sizes); `sdk/rocketnet_server_iocp_windows.cpp`
and `sdk/rocketnet_server_iocp.h`; `sdk/rocketnet_server_rio_windows.cpp`,
`sdk/rocketnet_server_rio.h` and `sdk/rocketnet_rio_layout.h`; the Windows branches of
`sdk/rocketnet_server_transport.cpp` and `sdk/rocketnet_client_transport.cpp`, and the
Windows operation tables, receive-drop hook and memory plan in `sdk/rocketnet_driver_builtin.cpp`
that select and drive the transports; the tests `tests/test_iocp.cpp`,
`tests/test_unit_windows_coalesced_cmsg.cpp`, `tests/functional/windows_connreset.cpp` and
`tests/functional/windows_socket_errors.cpp`; checked against `standard/TRANSPORT.md`,
`standard/MESSAGES.md`, `sdk/rocketnet.h` and the code's own comments. Method: for each
candidate, the call sequence or wire input that reaches it was written down first; a candidate
with no reachable path is graded risk or quality, never bug. Nothing was run on Windows: a
finding that only a Windows run could confirm says so. No code was changed. Findings are ordered
most severe first; each gives severity (`bug` / `risk` / `quality`), `file:line` at 9775266a, a
one-sentence defect, a one-sentence concrete failure or cost, and the smallest fix.

## Findings

### 1. bug — `sdk/rocketnet_server_iocp_windows.cpp:949`

- Defect: `rocketnet_iocp_send_bank` retires every frame of a bank, published or the final producer that stop hands over, as soon as `stopping` is set, so the TX worker never posts a send once `rocketnet_server_iocp_stop` has run, although the operations contract (`sdk/rocketnet_driver.h:78`) says flush stages the stop-time disconnect burst and the GSO transport's TX worker sends both after stop (`sdk/rocketnet_server_gso_linux.cpp:355`).
- Failure or cost: `rocketnet_server_stop` seals the disconnect burst, flushes it (which only publishes the bank and wakes the TX worker) and then calls stop at once, so on the default production Windows transport the TX worker wakes to `stopping == true` and counts the whole burst as `SEND_FAILED` instead of sending it; connected clients learn of the stop only by timing out, against `standard/TRANSPORT.md` t13.4 ("When a server stops it MUST disconnect every connected client with the redundant burst") (unconfirmed by run).
- Smallest fix: Drop the `stopping` test from `rocketnet_iocp_send_bank` so the published banks and the final producer are posted, and have the completion workers issue their `CancelIoEx` only after `send_exited` is true, cancelling receives by their own `OVERLAPPED` so in-flight burst sends complete rather than being cancelled.

### 2. bug — `sdk/rocketnet_server_rio_windows.cpp:464`

- Defect: The stop pass sets `send_ready_count = 0`, discarding every phase main handed but the worker had not yet taken, and the open fill phase is never handed after stop, with no counter for either.
- Failure or cost: The same `rocketnet_server_stop` sequence on the RIO transport hands the disconnect-burst phase and sets `stopping` before the woken worker reaches its next pass, whose first check turns it to the stop pass, so the burst is never posted and no counter (not `SEND_FAILED`, not `SEND_FULL`) records the loss, against t13.4 as in finding 1 (unconfirmed by run).
- Smallest fix: In the stop pass, post and commit each handed phase (and the finished slots of the fill phase) before `rocketnet_rio_close_socket`, or, at the least, add their finished-slot counts to `ROCKETNET_RIO_COUNTER_SEND_FAILED` before clearing them.

### 3. bug — `sdk/rocketnet_driver_builtin.cpp:1275`

- Defect: `rocketnet_server_transport_io_receive_drops` reports the transport's own size drops only for the Linux `gso` and `io_uring` tables and returns false on Windows, while the IOCP transport drops oversize, failed and zero-byte receives into its private `ROCKETNET_IOCP_RECEIVE_DROPPED` and the RIO transport drops oversize and below-minimum datagrams into its private `ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_SIZE` (`sdk/rocketnet_server_rio_windows.cpp:860`), contradicting the comment at `sdk/rocketnet_server_transport.cpp:3373` that one oversize datagram reads `PREFILTER_TOO_LARGE` on every transport.
- Failure or cost: An oversize datagram from the wire, or on RIO a runt below `ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES`, moves no counter that `rocketnet_server_transport_counters` returns on `iocp` or `rio`, while the same datagram reads `PREFILTER_TOO_LARGE` or `PREFILTER_TOO_SHORT` on `portable`, `gso` and `io_uring`; the functional case reaches into the transports' private counters through a test-only accessor to see them (`tests/functional/windows_socket_errors.cpp:302`). Class: data dropped with no counter.
- Smallest fix: Add `iocp_receive_drops` and `rio_receive_drops` that split their size drops into too-short and too-large totals, and match their operation tables in `rocketnet_server_transport_io_receive_drops` under a Windows branch.

### 4. risk — `sdk/rocketnet_server_iocp_windows.cpp:684`

- Defect: The IOCP socket options (lines 677–685) and the RIO socket options (`sdk/rocketnet_server_rio_windows.cpp:631`–639) set exclusive use, dual stack and buffer sizes but never the don't-fragment option, which the portable Windows socket (`sdk/rocketnet_platform.cpp:4726`), the `gso` and `io_uring` sockets and the XDP frames all set.
- Failure or cost: On a path whose MTU is below a packet's size, the two native Windows server transports send IP fragments where every other transport, including the portable transport on the same Windows host, sends a DF datagram that the path drops, so loss and path-MTU behaviour depend on which Windows rung the ladder granted (unconfirmed by run).
- Smallest fix: Set `IPV6_DONTFRAG` (IPv6 socket) or `IP_DONTFRAGMENT` (IPv4 socket) in both creates before `bind`, tolerating refusal as the portable socket does.

### 5. risk — `sdk/rocketnet_platform.cpp:4713`

- Defect: The Windows blocking socket's receive timeout is `DWORD( timeout_seconds * 1000.0f )`, which truncates any positive timeout below one millisecond to 0, the `SO_RCVTIMEO` value meaning no timeout, and converts a float above `UINT32_MAX` milliseconds to `DWORD` with undefined behaviour, while the header (`sdk/rocketnet_platform.h:289`) promises that positive values request an OS timeout.
- Failure or cost: A caller of the public platform layer that creates a `ROCKETNET_PLATFORM_SOCKET_BLOCKING` socket with a 0.5 ms timeout gets a receive that blocks forever on Windows instead of returning after the timeout; no in-tree caller creates a blocking socket today.
- Smallest fix: Round up with `ceil`, clamp to the range 1 .. `0xFFFFFFFE`, then set the option.

### 6. risk — `sdk/rocketnet_platform.cpp:4916`

- Defect: Under `ROCKETNET_RECEIVE_TEST_HOOKS`, a hook that reports drained (a negative result) makes `rocketnet_platform_socket_receive_datagram` return 0, although the function's contract (lines 4876–4881) is -1 for drained and 0 for a kept zero-length datagram.
- Failure or cost: On the Windows batch path (`keep_empty` true, `sdk/rocketnet_platform.cpp:5304`), a hooked drain reads as a zero-length datagram, so `rocketnet_platform_socket_receive_packets` fills all `max_packets` entries with 0 bytes and unwritten source addresses; only the per-packet path uses the hook today.
- Smallest fix: Return -1 for a negative hook result, as the Winsock drain path does at line 4948.

### 7. quality — `sdk/rocketnet_server_rio_windows.cpp:42`

- Defect: `rocketnet_rio_leaked_regions` is declared, commented as "regions destroy could not drain and so leaked", and read through `rocketnet_server_rio_leaked_regions`, but nothing ever increments it, and its own header comment (`sdk/rocketnet_server_rio.h:98`) calls it historical.
- Failure or cost: `tests/test_iocp.cpp:1064` and five functional cases (`tests/functional/rocketnet_functional_tests.cpp:4185` and after) assert the counter unchanged, checks that pass whatever destroy does and read as leak coverage that does not exist.
- Smallest fix: Delete the counter, its accessor and the checks, or replace the checks with an allocation or handle count taken around create and destroy.

### 8. quality — `sdk/rocketnet_server_rio_windows.cpp:665`

- Defect: Two remnants of the earlier two-phase send design remain after the move to `ROCKETNET_RIO_SEND_PHASES` (3): the request-queue comment says it "holds every send of both phases", and create stores zero into `next[0]` and `next[1]` only (lines 595–596).
- Failure or cost: A reader checking queue capacity or phase initialisation is told there are two phases and must re-derive that the queue is sized for `ROCKETNET_RIO_SEND_FRAMES` (all three) and that `next[2]` is zero only by value-initialisation.
- Smallest fix: Say "every send of every phase" in the comment and delete the two redundant stores (the member is already `{}`-initialised).

### 9. quality — `sdk/rocketnet_rio_layout.h:155`

- Defect: `rocketnet_rio_context` packs the phase as `(uint16_t) phase << 48`, sixteen bits that overlap the kind byte at bits 56–63, while its comment (line 151) and `rocketnet_rio_context_phase` treat the phase as the eight bits 48–55.
- Failure or cost: The layout header is the unit-tested byte-level contract, and a phase of 256 or more would silently rewrite the request kind, so a receive context could read as a send; only phases 0–2 exist today.
- Smallest fix: Cast the phase to `uint8_t` as the kind already is.

### 10. quality — `sdk/rocketnet_platform.cpp:5031`

- Defect: `get_connection_type` returns early when `addresses` is NULL (line 5025) and then tests `if ( addresses )` twice more (lines 5031 and 5048).
- Failure or cost: The dead checks suggest a NULL adapter list can reach the classification and the free, so a reader re-derives the early return to see that the "default to wired" branch always runs.
- Smallest fix: Set `result = ROCKETNET_CONNECTION_TYPE_WIRED` unconditionally after the early return and free `addresses` without the test.

## Counts

- bug: 3 (findings 1–3)
- risk: 3 (findings 4–6)
- quality: 4 (findings 7–10)
- total: 10

## Already filed

None of the findings above repeats a finding of `docs/audits/2026-10-24-code-quality-a.md`
or `docs/audits/2026-10-24-code-quality-b.md`, the two audits present at 9775266a, and
neither audit files a finding inside this area's files.
