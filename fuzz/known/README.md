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
  entry) for the ledger to balance.
