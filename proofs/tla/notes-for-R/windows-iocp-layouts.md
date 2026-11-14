# Notes for package R: windows-iocp-layouts (M12a)

Branch `models/v1.1.x/windows-iocp-layouts`. The items below belong to package
R or package C; this package did not make them.

## Roadmap entries (docs/roadmap.sexp, group "proof-debt")

```
  (item "trace-iocp-admit-real-bank" :group "proof-debt" :area "proofs"
   :title "Trace layer iocp-admit drives only the verdict"
   :text "tests/tla_trace_iocp.cpp calls rocketnet_iocp_receive_admit through rocketnet_server_iocp_test_receive_admit; the bank, the drop tally and the delivered counts are the walk's own. rocketnet_iocp_bank_datagram (num_packets++, the tally) and rocketnet_server_iocp_receive_packets are never called, so InBounds, Accounted and EveryDropCounted over the code's own bank rest on reading. Bind them: a development seam that builds a batch, runs rocketnet_iocp_bank_datagram on it and reads num_packets and the tally back into the record, and a drain through the receive handoff. Audit rows MM175, MM083 (README part)."
   :date "2026-11-13" :origin "proofs/tla/README-RNIocpReceiveAdmit.md, What the trace binds; MM175")

  (item "trace-iocp-lanes-real-post" :group "proof-debt" :area "proofs"
   :title "Trace layer iocp-lanes drives the lane arithmetic, not the bank and post"
   :text "tests/tla_trace_iocp_lanes.cpp drives the code's ref queue functions over a test set of free queues (rocketnet_server_iocp_test_lanes_*) and the lane, op-lane and send-thread arithmetic, and reads the free queues back after each step; the banks, the in-flight set, the posted orders and zeroed are the walk's own, and start_packet, finish_packet, abort_packet, rocketnet_iocp_send_bank, the gather, the post and rocketnet_iocp_release_send are never called. Bind them: seams over a real rocketnet_iocp_lane_t that run start_packet, finish and abort, the send bank's zero-byte release and post, and rocketnet_iocp_retire_send, with the record read from the lane. Audit rows MM176, MM177."
   :date "2026-11-13" :origin "proofs/tla/README-RNIocpSendLanes.md, What the trace binds; MM176")

  (item "trace-rio-lane-real-start" :group "proof-debt" :area "proofs"
   :title "Trace layer rio-lane drives the lane arithmetic, not start, finish and post"
   :text "tests/tla_trace_rio_lane.cpp calls the LaneOf, Encode and Decode functions of sdk/rocketnet_rio_layout.h directly; the slots, the next counter, slot_bytes and the post walk are the walk's own. rocketnet_server_rio_start_packet, finish_packet, abort_packet, rocketnet_rio_send_packets and rocketnet_rio_post_phase are never called. Bind them with seams over a rocketnet_rio_lane_t that run those paths with RIOSendEx replaced by a recorder. Audit row MM187."
   :date "2026-11-13" :origin "proofs/tla/README-RNRioLaneLayout.md, What the trace binds; MM187")
```

## proofs/tla/README.md and docs/TLA-INVENTORY.md

docs/TLA-INVENTORY.md has no row naming `RNIocpSendLanes`, `RNRioLaneLayout`
or `RNIocpReceiveAdmit`. Suggested row for the inventory table:

```
| Windows send lanes and admission | `RNIocpSendLanes`, `RNRioLaneLayout`, `RNIocpReceiveAdmit` | every IOCP send operation in one place, aborted (zero-byte) slots released unposted, each destination's packets posted in start order; the RIO lane of a hash in range, a finish reaching only its own reserved slot, a lane's frames posted in slot order; the prefilter before the capacity check, every drop counted |
```

The `RNIocpRetryStop` line (the two "IOCP server queues and send leases" rows of docs/TLA-INVENTORY.md) still holds.

## Comment re-citations for package C (sdk/, comment only)

- `sdk/rocketnet_server_iocp_windows.cpp` (rocketnet_iocp_receive_admit), the comment above it, line 294 at 084cb20af
  now: `// proofs/tla/RNIocpReceiveAdmit.tla Arrived: Refuse, then Bank or Full.`
  new: `// proofs/tla/RNIocpReceiveAdmit.tla Refuse, then Bank or Full: the verdict.`
  (main renamed Arrive to Arrived, the model's shared conjunct of the three
  actions, not an action; naming the actions is clearer; optional)
- `sdk/rocketnet_server_iocp_windows.cpp` (rocketnet_iocp_receive_admit), the plant comment inside it, line 300 at 084cb20af
  now: `// first, the order the blast row measured. Its trace must fail.`
  new: `// first, the order this code replaced. Its trace must fail.`
- `sdk/rocketnet_server_iocp_windows.cpp` (rocketnet_iocp_bank_datagram), the comment above it, line 1405 at 084cb20af
  now: `... proofs/tla/RNIocpReceiveAdmit.tla Arrived: Bank, or the`
  new: `... proofs/tla/RNIocpReceiveAdmit.tla Bank, or the`
  (line 1406 keeps `count of Refuse and Full (EveryDropCounted).`)
- `sdk/rocketnet_server_rio_windows.cpp` (rocketnet_rio_post_phase), the last line of the comment above it, line 1863 at 084cb20af
  now: `// proofs/tla/RNRioLaneLayout.tla Send: a lane's worker posts the frames staged in its own lane, oldest first.`
  new: `// proofs/tla/RNRioLaneLayout.tla Send: a lane's worker posts the finished frames of its own lane in slot order (InOrder).`

## Text in another module's README (MA035)

`proofs/tla/README-RNIocpRefQueue.md`, the paragraph on `NeverRefused`, says
the one-consumer case is "`retry_receives` under one completion worker, the
count production runs". The default is `ROCKETNET_IOCP_DEFAULT_COMPLETION_WORKERS`,
2, on a specific bind (`sdk/rocketnet_network_threads.h`), and one worker only
on a wildcard bind with no count asked (`iocp_workers` in
`sdk/rocketnet_driver_builtin.cpp`). Suggested text: "That is
`retry_receives` under one completion worker, the count on a wildcard bind
with no count asked; the default on a specific bind is two workers, each a
consumer (`MCRNIocpRefQueueRefusedTwoConsumers.cfg`)." The cfg's own comment
(`MCRNIocpRefQueueOneConsumer.cfg`) is corrected on this branch.
