# Notes for package R (and C) from M12b, windows-rio-worker

Branch `models/v1.1.x/windows-rio-worker`, on integration/v1.1.54 (4f2960e5f)
with fix/v1.1.x/windows-receive-handoff-latency 8c58f1946 merged. Modules:
RNRioWorker, RNRioDriver. Rows: MM181-MM186, MM191, MM219, MM221.

## Roadmap entries (sexp text for docs/roadmap.sexp; not edited here)

```
  (item "rio-worker-trace-walk" :group "review-v1-2" :area "proofs"
   :title "Bind RNRioWorker to the rio transport with a trace walk"
   :text "proofs/tla/RNRioWorker.tla has no trace binding: the unit seams in sdk/rocketnet_server_rio_windows.cpp (rocketnet_server_rio_test_worker_handoff, rocketnet_server_rio_test_stop_drains_sends, rocketnet_server_rio_test_create_failure_drain, rocketnet_server_rio_test_create_failure_unposted) drive the real worker pass, main calls and stop pass against a fake kernel, but no tests/tla_trace_*.cpp walk emits those steps for TLC to check against the spec. Add a walk over the same seams (worker pass, receive call, send flush, stop pass, create's inline drain including the abandon branch) with a trace module, as the other transports have, and a plant. Models review MM184 (trace part); no defect shown."
   :date "2026-11-13" :origin "models review MM184, package M12b")
```

## proofs/tla/README.md and docs/TLA-INVENTORY.md lines

- RNRioWorker: 25 cases now (was 22): add MCRNRioWorkerInlineStopUnposted.cfg
  (pass), MCRNRioWorkerBrokenLeakUnposted.cfg (LeakOnlyAfterStalls) and
  MCRNRioWorkerReachAbandonFreed.cfg (NotReachedAbandonFreed, reachability).
  Wherever the README or the inventory counts the RNRioWorker cases or lists
  its witnesses, update it. Its line should also say: no trace binding (unit
  seams only; roadmap rio-worker-trace-walk).
- RNRioDriver: historical design only (MM191). The header and README now say
  so; COVERAGE rows rio-driver and rio-transport already say "Historical
  caller-driven design; no current code implements it" with status owed.
  The TLA-INVENTORY line should read historical, not current.

## COVERAGE rows rio-driver and rio-transport (MM191 records part, R's)

- The two rows are near duplicates of one module (Ar AM-09); rio-transport
  omits ReceiveCallBound and MCRNRioDriverBrokenUnboundedReceive.cfg that
  rio-driver lists. Recommend one row (rio-driver) for the historical module,
  or both rows identical in their invariant and witness lists. Status stays
  owed with the historical sentence (not current: no code implements it).
- tools/v1.1.x/trace-layers-at-scale 3d658b038 (parked, PR after v1.2.0)
  deletes RNRioDriver whole in 9b95b845d ("Delete five historical TLA+
  modules"): the module, MCRNRioDriver.tla, README-RNRioDriver.md, the two
  COVERAGE rows, the rnriodriver CASES rows, and moves the functional-test
  citation below to RNRioWorker. MM191 is therefore closed when that branch
  lands, not before; until then this branch's header correction stands. When
  both land, the deletion wins (drop this branch's RNRioDriver.tla and
  README-RNRioDriver.md hunks).

## Citations for package C (comment lines only)

- tests/functional/rocketnet_functional_tests.cpp:5223 at this branch's
  merged base (the table's :5214 is at 5a2dbf403) now reads
  `// the rio transport (sdk/rocketnet_server_rio_windows.cpp, proofs/tla/RNRioDriver.tla)`.
  New text:
  `// the rio transport (sdk/rocketnet_server_rio_windows.cpp, proofs/tla/RNRioWorker.tla)`.
  (The same edit is in trace-layers-at-scale 9b95b845d; whichever lands
  first, the other is a no-op.)
- sdk/rocketnet_server_rio_windows.cpp:1240-1254 (the abandon branch of
  rocketnet_rio_create_failed_drain) has no model citation of its own; the
  function's comment at :1221-1228 cites RNRioWorker WLeak, which now models
  that branch too. Optional new comment line at :1242, before "closesocket
  refused on every pass": `// proofs/tla/RNRioWorker.tla WLeak, AbandonUnposted:`
  (keep the existing text after it).

## Cross-family

- MM221 and MM219 cite code that does not exist in this repository at any of
  5a2dbf403, 4f2960e5f or 8c58f1946 (rocketnet_server_rio_post_send_bank,
  recv_handoff, rx_bank_capacity; send_handoff exists only in the XDP code,
  sdk/rocketnet_server_socket_linux.cpp). MM220 (M12a, RNRioSendSlotLayout)
  comes from the same Argon finding AM-02 and names the same send_handoff,
  tx_lanes and tx_bank_counts: worth checking against the RIO code before it
  is accepted.
