# Notes for R and C from package M5b (send-memory)

Base integration/v1.1.54 at 4f2960e5. Line numbers are at the base.

## For C: comment-only re-citations in sdk/ and tests/

MM068, sdk/rocketnet_allocator.cpp:947-950, the RNSendSpans citation block
names two of the eleven cases. New text for lines 947-950:

    // This representation (offsets in 0..SR-1, head/tail, the first..last list)
    // is modelled in proofs/tla/RNSendSpans.tla (proofs/tla/CASES.tsv group
    // rnsendspans): the controls MCRNSendSpans.cfg, MCRNSendSpansRefuse.cfg and
    // MCRNSendSpansAbort.cfg, and the reversed witnesses
    // MCRNSendSpansBrokenFloorAtNewest.cfg, MCRNSendSpansBrokenNoPadding.cfg,
    // MCRNSendSpansBrokenHalfSplice.cfg, MCRNSendSpansBrokenRefuseExhausts.cfg,
    // MCRNSendSpansBrokenRefuseChanges.cfg, MCRNSendSpansBrokenLinkFirst.cfg
    // and MCRNSendSpansBrokenNoUndo.cfg.

MM070: RNConnectionMemory's Reuse is now only the O(1) free
(rn_message_connection_release); the carve is Connect's (rn_endpoint_reset,
rn_region_reset when the slot is bound). The citations that name Reuse for
the carve move to Connect:

- sdk/rocketnet_allocator.cpp:591: `// proofs/tla/RNConnectionMemory.tla, Reuse: the slate is carved clean`
  becomes `// proofs/tla/RNConnectionMemory.tla, Connect: the slate is carved clean before the slot's next client`
- sdk/rocketnet_allocator.h:507: `// (RNConnectionMemory Reuse): the carve, so a reused slot starts exactly as`
  becomes `// (RNConnectionMemory Connect, after Reuse freed the slot): the carve, so a reused slot starts exactly as`
- sdk/rocketnet_endpoint.cpp:241: `// proofs/tla/RNEndpoint.tla Reset (RNSlots ResetFree, RNConnectionMemory Reuse):`
  becomes `// proofs/tla/RNEndpoint.tla Reset (RNSlots Observe, RNConnectionMemory Connect):`
  (RNSlots: ResetFree keeps the region, Observe carves it.)
- sdk/rocketnet_message_server.cpp:181: `// RNSlots ResetFree, and RNConnectionMemory Reuse: the region carved again and`
  becomes `// RNSlots Observe, and RNConnectionMemory Connect: the region carved again and`
- sdk/rocketnet_message_server.cpp:830: `// proofs/tla/RNConnectionMemory.tla Reuse, whose one step this splits by a stutter):`
  becomes `// proofs/tla/RNConnectionMemory.tla Reuse, the O(1) free; the carve is the next Connect's):`
- sdk/rocketnet_message_object.h:462-464: `// R5: ends a connection with `reason`. The transport disconnects it now; its`
  `// region is reset at the start of the next update, before the slot can be`
  `// reused (RNConnectionMemory Kick, then Reuse).` becomes
  `// R5: ends a connection with `reason`. The transport disconnects it now; its`
  `// slot is freed in O(1) and its region carved when the slot is bound to its`
  `// next connection (RNConnectionMemory Kick, Reuse, then Connect).`
- tests/test_unit_connection_memory.cpp:428: `// RNConnectionMemory Reuse and ReusedSlotStartsClean: a slot's region, used`
  becomes `// RNConnectionMemory Connect and ReusedSlotStartsClean: a slot's region, used`
- tests/test_unit_connection_memory.cpp:907: `// RNConnectionMemory Reuse: the reset clears the sticky state, and the span`
  becomes `// RNConnectionMemory Connect: the carve clears the sticky state, and the span`
- tests/test_unit_message_objects.cpp:2043: `// RNSlots ResetFree, RNConnectionMemory Reuse: a reused slot starts exactly as`
  becomes `// RNSlots ResetFree and Observe, RNConnectionMemory Reuse and Connect: a reused slot starts exactly as`

MM065 area (handed up as code-wrong, codewrong/send-memory-MM065.md): if the
lead takes the claim fix rather than a code fix,
sdk/rocketnet_server_transport.cpp:4797-4801 and
tests/test_unit_server_send_reserve.cpp:729-734 narrow "the slots a thread
takes only rise" to "a lost install never makes a thread's slots fall" and say
that a chunk claimed before the guard and installed after it can give a thread
a slot below the single slots it took (the file has the trace).

## For R: docs/TLA-INVENTORY.md

- Line 72 (server payload ring), the claim column becomes:
  `no slot at or past the send capacity handed out or read, each flush sends exactly the slots finished since the last (a lost chunk install and every line's unused chunk marked empty), a slot held by one thread at its own frame, a line's chunk below the capacity and the counter`
- Line 34 (send store and descriptor pool): "a refusal or connection kick
  only when exhausted" is stale since revision 1.1 (a send with no room is
  refused and changes nothing; only the receive side exhausts). It becomes:
  `no chunk or descriptor handed out while live, exact free counts, free stack and span ring, a refused send changing nothing, a kick only on the receive side's exhaustion, no read of never-taken memory, live generations never 0 across a wrap`

## For R: proofs/tla/README.md

No line changes: the group rows for rnsendstore, rnpacketlease and
rnserverpayloadringlayout name their READMEs, which carry the changes.

## Roadmap entries (sexp text, for docs/roadmap.sexp)

Only if the lead does not take the code-wrong file into a fix now:

      (item "payload-ring-slots-rise" :group "after-v1-1-landing" :area "sdk"
       :title "Payload ring: keep one thread's slots rising across the fill guard"
       :text "rocketnet_server_payload_ring_take (sdk/rocketnet_server_transport.cpp) says the slots a thread takes only rise, so the flush sends one client's packets in start order. A chunk claimed before the guard and installed after it breaks that: two threads on one fill line, send_capacity 512 to 519; one claims 0..7 and stops before its install; the other finds the line empty, the guard trips at 8, and it takes 8 and 9 by the single fetch-add; the install lands, and the second thread's next start takes 1 from the line. Read against the code, not run. Fix: narrow the claim in the code and test comments, or keep the line's late chunk from takers once a single take has happened on it; then RNServerPayloadRingLayout gains SlotsRise with the guard path as its reversed witness."
       :date "2026-11-13" :origin "Models fix round, package M5b (send-memory), found correcting MM065")
