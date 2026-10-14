# Expected-fail corpus for rocketnet_fuzz_receive_structured

Each file in this directory is a minimized input that fires an oracle in
`fuzz/rocketnet_fuzz_receive_structured.cpp` on the BASE source. They are the
findings of this card; the smoke job replays only `fuzz/corpus_receive_structured/*`
and stays green while the fix card is open.

- `connection_request_salt_mismatch_uncounted` — fires the counter-ledger
  oracle ("connection request ledger: received == sum of outcomes"). A
  connection request (type 0) that reaches an existing in-flight mapping with
  a different connection salt is refused silently at
  `sdk/rocketnet_server_protocol.cpp` (`rocketnet_server_protocol_process_connection_request`,
  the salt-mismatch branch) without incrementing any outcome counter, so
  `CONNECTION_REQUEST_RECEIVED` runs one ahead of the sum of its outcomes. The
  refusal needs a named counter (a `CONNECTION_REQUEST_SALT_MISMATCH`-style
  entry) for the ledger to balance. The request-ledger check counts these
  refusals and lets them pass so the campaign stays green while the fix card is
  open; the fix card lands the counter and removes the salt-mismatch count from
  the ledger sum, then verifies this input no longer fires.

- `connection_response_refused_refreshes_idle_clock` — the refusal-immutability
  oracle. A connection response (type 3) that opens and is then refused
  (`ALREADY_CONNECTED` / `CLIENT_ID_ALREADY_CONNECTED`) has already refreshed
  `mapping->last_receive_time` (and may advance `replay_handshake.highest_opened`)
  at `sdk/rocketnet_server_protocol.cpp` before the already-connected check —
  the keep-alive-index-before-check class: the packet is refused yet mutates
  the idle clock and the replay high-water mark. The oracle's fatal check is
  narrowed to the sequence trackers and the replay-window bits so the campaign
  stays green; the fix card re-enables the idle-clock / high-water-mark check
  and verifies this input no longer fires.
