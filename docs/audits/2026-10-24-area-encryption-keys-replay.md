# Packet encryption, keys and replay protection audit, 2026-10-24

Cold reading audit of `mas-bandwidth/rocketnet` at `9775266a705148969cc00fcadc1463b06879cfbd`
(`main`). Scope read, completely: `sdk/rocketnet_encryption_map.{h,cpp}`,
`sdk/rocketnet_keys.{h,cpp}`, `sdk/rocketnet_replay.{h,cpp}`,
`sdk/rocketnet_server_replay_marks.{h,cpp}`, `sdk/rocketnet_crypto_tally.h`, the seal and
open paths of `sdk/rocketnet_protocol.cpp`, `sdk/rocketnet_server_protocol.cpp` and
`sdk/rocketnet_client_protocol.cpp` (sealed-packet processing, the integrity limit, session
sends, cookie key rotation, the mapping sweep), the kernel replay check in
`server_xdp/server_xdp.c`, and the tests that pin them
(`tests/test_unit_standard_packet_protection.cpp`, `tests/test_unit_standard_replay.cpp`,
`tests/test_unit_replay_trace.cpp`, `tests/test_unit_replay_window_bounds.cpp`,
`tests/test_unit_server_replay_marks.cpp`, `tests/test_unit_cookie_key_rotation.cpp`),
against `standard/TRANSPORT.md` t7 (key schedule), t9 (packet protection), t10 (receive
processing) and t14 (replay protection). Connect tokens and the handshake rungs before the
encryption mapping exists are out of scope. Method: each candidate was traced to an input or
call sequence that reaches it; one with no reachable path is graded risk or quality, never
bug. Findings already filed in `docs/audits/` at this sha are listed once at the end, not
repeated. No code was changed. Findings are ordered most severe first; each gives severity
(`bug` / `risk` / `quality`), `file:line` at 9775266a, a one-sentence defect, a one-sentence
concrete failure or cost, and the smallest fix.

## Findings

### 1. bug — `sdk/rocketnet_server_protocol.cpp:1189`

- Defect: A connection response that opens under the handshake key but carries non-zero padding is counted as `CONNECTION_RESPONSE_DID_NOT_OPEN` (line 1188) and charged to the mapping's integrity-failure count (line 1189), while `standard/TRANSPORT.md` t9 rule 8 says the count MUST NOT include a packet that opens and then fails a later structural check, and t10 step 13 names only a failed open or a wrong handshake plaintext size as `DID_NOT_OPEN`.
- Failure or cost: A client holding the handshake key that sends a response with any non-zero padding byte (a non-conforming implementation, reachable over the wire today) has authentic packets reported as forgeries and counted toward the integrity limit, so an operator reads an interoperability bug as an attack.
- Smallest fix: Drop the `rocketnet_server_integrity_failure` call on the padding branch and count the refusal under its own structural counter rather than `DID_NOT_OPEN` (or, if non-zero padding is meant to count as a failed open, say so in t9 rule 8 and t10 step 13).

### 2. risk — `sdk/rocketnet_replay.cpp:67`

- Defect: For a forward jump shorter than one window depth, `rocketnet_replay_advance` clears the skipped sequences one bit at a time, up to W − 1 iterations (4095 at the default depth, 32767 at the maximum), each a modulo and a read-modify-write.
- Failure or cost: An authenticated sender that advances its sequence by W − 1 on every packet (a hostile client toward the server, or a hostile server toward a client) makes each opened packet pay thousands of loop iterations on the receive path, and every legitimate loss burst pays the same per skipped sequence.
- Smallest fix: Clear the partial words at each end of the skipped range bit by bit and every whole word between with one store, bounding the work at W / 64 word stores plus two partial words.

### 3. risk — `sdk/rocketnet_client_protocol.cpp:803`

- Defect: The client's session open passes `packet_bytes` as `out_capacity`, so the capacity guard in `rocketnet_open_sealed_packet` (`sdk/rocketnet_protocol.cpp:321`) compares the plaintext size with the packet it came from and always passes; the real bound on `opened` (`ROCKETNET_RECEIVE_CEILING` at the callers, e.g. line 1095) is enforced only upstream by the prefilter (`sdk/rocketnet_protocol.cpp:94`).
- Failure or cost: A future caller that hands the session path a buffer smaller than the ceiling, or a receive path that reaches it without the prefilter, writes past `opened` by the difference with no refusal, where the server's open (`sdk/rocketnet_server_protocol.cpp:1155`) would refuse.
- Smallest fix: Pass `ROCKETNET_RECEIVE_CEILING` as the server does, or thread the buffer's capacity through `rocketnet_client_session_entry` to the open.

### 4. quality — `sdk/rocketnet_encryption_map.h:85`

- Defect: Each mapping carries a full `rocketnet_replay_protection_t replay_handshake` (520 bytes at the default depth, 4104 at the maximum), but only its `highest_opened` is ever read or written (`sdk/rocketnet_server_protocol.cpp:1109`, `1112`, `1195`); the window bits are dead because the handshake types have no replay test (t10 step 10), and the client's copy (`sdk/rocketnet_client_protocol.h:84`) is the same.
- Failure or cost: 512 dead bytes per mapping, zeroed on every add, with two mappings per client slot: about 1 MB at 1000 slots at the default depth and about 8 MB at the maximum, spent for no bandwidth or CPU saving.
- Smallest fix: Replace the field with a `uint64_t handshake_highest_opened` in both structs and update the RESET inventory in `sdk/rocketnet_connection_reset.h` to the new name.

### 5. quality — `tests/test_unit_replay_trace.cpp:16`

- Defect: The file's header (lines 15–21), the trace's comment at line 242 and the witness banner at line 274 say the code clears only four words on a long skip, advances the session window before the slot gate, and that the two witnesses are registered as expected failures and red, while the code clears the whole ring (`sdk/rocketnet_replay.cpp:63`), gates before the advance (`sdk/rocketnet_server_protocol.cpp:1143`), the trace runs with every word cleared (line 259), and both witnesses are plain registrations at lines 378–379.
- Failure or cost: A maintainer reading the test is told the replay window still has two open defects and that the trace pins the defective behaviour, the opposite of what the test now checks, and the cited line numbers no longer point at the code they name.
- Smallest fix: Rewrite the three comments to state the rules as now implemented (ClearedWords = Words, GateBeforeAdvance = TRUE) and drop the stale line references.

### 6. quality — `sdk/rocketnet_encryption_map.h:33`

- Defect: Comments in this area cite sections of `STANDARD.md` that no longer exist there (`_Half-open state_` here and at `sdk/rocketnet_server_protocol.cpp:1575`, `Replay Protection` at `sdk/rocketnet_replay.cpp:61`, the key schedule at `sdk/rocketnet_keys.h:13`), while `STANDARD.md` is now a 14-line index and the rules live in `standard/TRANSPORT.md` (t7, t12 rule 14, t14, t17 rule 4).
- Failure or cost: A reader following a cite to check the code against its rule lands on an index with no such section and has to search the standard to find the rule the code claims to implement.
- Smallest fix: Replace each `STANDARD.md` section name with the `standard/TRANSPORT.md` section id it means.

### 7. quality — `sdk/rocketnet_encryption_map.cpp:128`

- Defect: The create comment says the address, control and mapping arrays are "3.4 KB an entry", while at this sha they are 20 + 32 + 2184 = 2236 bytes an entry at the default replay depth (about 13 KB at the maximum depth), measured with `sizeof`.
- Failure or cost: The figure a maintainer would use to judge the cost of zeroing at create, or of sizing the table, is half again too large at the default depth and nearly four times too small at the maximum.
- Smallest fix: State the size as a function of `ROCKETNET_HISTORY_PACKETS`, or drop the number and keep the reason.

## Counts

- bug: 1 (finding 1)
- risk: 2 (findings 2–3)
- quality: 4 (findings 4–7)
- total: 7

## Already filed

- `sdk/rocketnet_protocol.cpp:421`, the dead `best_distance` store in `rocketnet_reconstruct_near`, which the sequence reconstruction ahead of every open calls: `docs/audits/2026-10-24-code-quality-a.md` finding 15.
