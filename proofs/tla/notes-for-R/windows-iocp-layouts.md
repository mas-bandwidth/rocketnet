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

The `RNIocpRetryStop` line (TLA-INVENTORY.md:61 and :89) still holds.

## Comment re-citations for package C (sdk/, comment only)

- `sdk/rocketnet_server_iocp_windows.cpp:294`
  now: `// proofs/tla/RNIocpReceiveAdmit.tla Arrive: Refuse, then Bank or Full.`
  new: `// proofs/tla/RNIocpReceiveAdmit.tla Refuse, then Bank or Full: the verdict.`
  (the module has no action Arrive)
- `sdk/rocketnet_server_iocp_windows.cpp:300`
  now: `// first, the order the blast row measured. Its trace must fail.`
  new: `// first, the order this code replaced. Its trace must fail.`
- `sdk/rocketnet_server_iocp_windows.cpp:1405`
  now: `... proofs/tla/RNIocpReceiveAdmit.tla Arrive: Bank, or the`
  new: `... proofs/tla/RNIocpReceiveAdmit.tla Bank, or the`
  (line 1406 keeps `count of Refuse and Full (EveryDropCounted).`)
- `sdk/rocketnet_server_rio_windows.cpp:1863`
  now: `// proofs/tla/RNRioLaneLayout.tla Send: a lane's worker posts the frames staged in its own lane, oldest first.`
  new: `// proofs/tla/RNRioLaneLayout.tla Send: a lane's worker posts the finished frames of its own lane in slot order (InOrder).`

## Records in other modules' files

`proofs/tla/RNIocpReceiveAdmit.tla` (lines 27, 36, 50, 162) and
`MCRNIocpReceiveAdmitBrokenBankFirst.cfg:1` still say "the blast row"; that
module is not in this package's files. Suggested text: "the order the code
replaced (capacity check first)".
