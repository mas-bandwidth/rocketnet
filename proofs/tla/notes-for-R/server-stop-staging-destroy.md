# Notes for R and C from M6 (server-stop-staging-destroy)

Branch `models/v1.1.x/server-stop-staging-destroy`. Modules: ServerStopSpread,
ServerSession, SessionSendSequence, RNDriverSendStaging, RNDestroyWipe.
Places are named by symbol, row id or quoted code text: check_tla_coverage
refuses line citations in proofs/tla.

## Roadmap entries (sexp, for docs/roadmap.sexp)

MM082: no trace walk binds ServerStopSpread.

```
(item "server-stop-spread-trace" :group "after-v1-1-landing" :area "proofs"
 :title "Bind ServerStopSpread to the code by trace"
 :text "ServerStopSpread (the disconnect ring's layout and the stop across steps, with send queues and the development link simulator) is checked by TLC at its small instance but no trace walk binds it: tests/test_unit_server_stop_spread.cpp drives the stop without emitting a trace the model checks. Enforcing it: a LAYERS row for ServerStopSpread, a writer in the stop step and the drain (ring head, count, cursor, each entry's sealed count, staged and in-flight per queue, held copies, phase) and a planted row whose trace the spec rejects. The layer stays unlocked until then."
 :date "2026-11-13" :origin "models audit MM082")
```

MM087: the XDP generation graph is not composed with the transport graph in
RNDriverSendStaging.

```
(item "driver-send-staging-compose-xdp" :group "after-v1-1-landing" :area "proofs"
 :title "Compose RNDriverSendStaging's XDP graph with its transport graph"
 :text "RNDriverSendStaging checks the transport's flush rule (a committed disconnect burst is handed to the driver's flush by the next send_packets) and, separately, XDP generation ownership (a retained producer is published by a later flush, under the assumption that the handoff lock is not held at every try). The two graphs interleave independently, so no state says which frames a retained producer holds, and the end-to-end claim that a committed disconnect leaves within a bounded number of flushes on XDP is not checked. Composing them: the transport's flush hands its frames to the XDP producer, Publish moves them to a published generation, WorkerRetire to the wire, and a liveness property from commit to wire under the lock assumption, at a size that stays inside the record budget."
 :date "2026-11-13" :origin "models audit MM087")
```

MM079: ServerStopSpread's drain pass over a full queue (`PassFull` in
SealOne) is modelled but has no witness of its own; the pooled-room witness
fails on the pooled abandon alone.

```
(item "server-stop-spread-pass-full-witness" :group "after-v1-1-landing" :area "proofs"
 :title "A reversed witness for the disconnect drain's pass over a full queue"
 :text "rocketnet_server_transport_drain_disconnect_bursts passes over a burst whose own send queue has no room while another queue has some; ServerStopSpread models that as SealOne's PassFull arm, but no property needs it: in the stop, the per-queue abandon drops the jammed queue's bursts and the drain goes on without the pass. Removing the pass is visible only on a running server, where nothing is abandoned and a burst on a queue with room would wait behind a jammed one. Enforcing it: a liveness property over RUNNING (a waiting burst whose queue keeps room is eventually sealed or the server stops), a nopass defect, and a two-queue witness that fails it."
 :date "2026-11-13" :origin "models audit MM079, cold read")
```

MM083, roadmap part: the roadmap's own text cites FABLE-AUDIT.md item
numbers (the header's `:text` and `:origin` fields such as
`"FABLE-AUDIT.md items 148, 185, 223, 226; ..."`). FABLE-AUDIT.md is an
excluded findings document; R decides whether those origins stay as history
labels or are replaced with source anchors.

## Shared record rows owned by other families (MM083, MM088)

- MM083: COVERAGE row `rn-send-fair` (RNServerSendFair) and the header
  comment of `proofs/tla/MCRNServerSendFairShort.cfg` cite "FIX-PROBE-SLOT
  section 3, closed by v1.1.2", an excluded findings document. Replace with
  the source anchor the claim rests on (the send pass's capacity test in
  `rocketnet_server_transport_send_capacity_available`,
  `sdk/rocketnet_server_transport.cpp`), owned by RNServerSendFair's family.
  The README-RNIocpReceiveAdmit.md part is M12's.
- MM088: COVERAGE row `send-handoff-server-finish` (RNDriverSendStaging).
  New note text:

  > rocketnet_server_socket_finish_packet writes the byte count of the packet
  > slot start_packet staged in the producer buffer, and refuses a packet
  > pointer outside the current producer buffer (rocketnet_server_socket_send_slot),
  > so a pointer from before the last flip is a lost packet and never
  > rewrites a published one. Not modelled: RNDriverSendStaging carries no
  > buffer identity, pointer, offset, slot or finish action (XdpStart stands
  > for a start with capacity left, Publish for the producer flip), and
  > removing the pointer check has no counterpart in the model. The check is
  > held by the unit tests of the finish path.

  Drop "Actions XdpStart and Publish." from that row together with the C
  re-citation below.

## README and inventory lines

- `proofs/tla/README.md`, the `serverstopspread` group row: "The disconnect
  ring's layout and the stop spread across steps within the send room, per
  send queue, with the development link simulator's held copies; one
  committed case (MCServerStopSpreadSimulatorJam.cfg) fails as the code
  does, an open code defect (README-ServerStopSpread.md)."
- `proofs/tla/README.md`, the `sessionsendsequence` group row: "The session
  send counter shared by fills, control sends and the server's deferred
  disconnect burst (README-SessionSendSequence.md)."
- `docs/TLA-INVENTORY.md`, the "endpoint reset and lifecycle" row
  (RNDestroyWipe): add "ring key copies of deferred disconnect bursts wiped
  when they leave and at destroy; destroy writes no page the run did not".
- `docs/TLA-INVENTORY.md`, the "deferred disconnect bursts" row: add "the
  stop waits for the link simulator to empty; a jammed queue with copies
  held is an open code defect (MM080)".

## Open code defect, cross-family (MM080)

`MCServerStopSpreadSimulatorJam.cfg` is committed with expected `temporal`
(`StopFinishesWithoutRoom`): the code cannot stop a transport whose queues
are jammed while the link simulator holds copies (the shutdown drain stops
at `io_send_room <= 0`, the fewest slots over the queues, and the I/O stop
needs `link_empty`). Owned by the send-queue-capacity follow-up. When the
code fix lands, the case's expected column turns to `pass` (or the case is
replaced by the fix's own control), with the model's simulator arms changed
to whatever the fix does with held copies.

## C: comment re-citations in sdk/

- `sdk/rocketnet_server_socket_linux.cpp`, the two comment lines above
  `rocketnet_server_socket_finish_packet` that begin
  `// proofs/tla/RNDriverSendStaging.tla XdpStart: finish writes` and
  `// proofs/tla/RNDriverSendStaging.tla Publish: a pointer outside` (MM088):
  replace them with
  `// rocketnet_server_socket_finish_packet: writes the byte count of the packet slot start_packet staged in the producer buffer (proofs/tla/RNDriverSendStaging.tla XdpStart);`
  `// a pointer outside the producer buffer (one from before the last flip) is refused; RNDriverSendStaging does not model the pointer check (COVERAGE send-handoff-server-finish).`
- `sdk/rocketnet_server_transport.cpp`, the comment block above
  `rocketnet_server_commit_staged` (MM089): the existing words "the staging
  slot goes back" are wrong for a staged driver, where io_abort_packet only
  zeroes the slot's byte count and the slot stays claimed until the flush.
  Replace that clause and add
  `// proofs/tla/RNDriverSendStaging.tla Commit (its link simulator arm): io_abort_packet disarms the slot, which stays claimed until the next flush hands the bank off (WithdrawnNeverSent, ClaimedFlushOwed).`
- `sdk/rocketnet_server_transport.cpp`, above
  `rocketnet_server_abort_send_frame` (MM089): add
  `// proofs/tla/RNDriverSendStaging.tla Update (its abort arm): a reserved frame never committed is disarmed, not given back; its slot is freed by the flush update's send_staged++ makes owed.`
- `sdk/rocketnet_server_transport.cpp`, in
  `rocketnet_server_transport_defer_disconnect_burst`, above
  `burst->first_sequence = rocketnet_platform_atomic_add_u64(` (MM086): add
  `// proofs/tla/SessionSendSequence.tla BurstTake: the burst's sequences in one atomic add on the counter fills and control sends share.`
  and above `memcpy( server_transport->disconnect_burst_keys[index], mapping->keys.session_server_to_client,` (MM093):
  `// proofs/tla/RNDestroyWipe.tla Remove (its deferred-burst arm): the session key copied into the ring outlives the mapping until the burst leaves.`
- `sdk/rocketnet_server_transport.cpp`, in
  `rocketnet_server_transport_drain_disconnect_bursts`, above the
  `rocketnet_write_sealed_packet` call that seals at
  `burst->first_sequence + (uint64_t) burst->sealed` (MM086): add
  `// proofs/tla/SessionSendSequence.tla BurstSeal.`
- `sdk/rocketnet_server_transport.cpp`, each
  `sodium_memzero( server_transport->disconnect_burst_keys[server_transport->disconnect_burst_head], ROCKETNET_KEY_BYTES );`
  (two in `rocketnet_server_transport_stop_step`'s abandon branches, one in
  the drain's pop of a complete head) (MM093): add
  `// proofs/tla/RNDestroyWipe.tla BurstLeaves.`
- `sdk/rocketnet_server_transport.cpp`, above `rocketnet_server_link_drain`
  (MM080): add
  `// proofs/tla/ServerStopSpread.tla Flush (the link drain arm): re-stages held copies while io_send_room, the fewest free slots over the queues, is above 0.`
  and in `rocketnet_server_transport_stop_step`, above
  `const bool link_empty =`:
  `// proofs/tla/ServerStopSpread.tla RequestIoStop: the I/O stop waits for the link simulator to hold nothing (NoCopyHeldAtStop).`

Once the SessionSendSequence and RNDestroyWipe citations land, R can add
COVERAGE rows for `sdk/rocketnet_server_transport.cpp` against those modules
and drop BurstTake, BurstSeal and BurstLeaves from their "Environment
actions" phrases.
