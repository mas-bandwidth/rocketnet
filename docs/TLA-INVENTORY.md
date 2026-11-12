# TLA+ inventory

The `proofs/tla/` directory contains executable specifications for RocketNet's
bounded protocol and message machines. `proofs/tla/CASES.tsv`,
`proofs/tla/RUNS.tsv` and `proofs/tla/COVERAGE.tsv` are the maintained case,
run and action-coverage ledgers: `tools/check_tla_records.py` validates the
ledger schema, exact one-to-one `(config, module)` declaration and record
mapping, expected outcomes and exit codes, and the pinned TLC JAR digest
(`jar_sha256`); `tlacheck groups --stale` and `tlacheck merge` check
current-input fingerprint freshness (`input_sha256` against `.tla` and `.cfg`
sources); and `tools/check_tla_coverage.py` validates module, action, symbol
and citation coverage in `proofs/tla/COVERAGE.tsv`.

This is a navigation inventory, not a claim that every model matches every
current implementation detail. `RNOutgoingBuild` models the shipped packed
design of the [unreliable aggregation path](OUTGOING-BUILD.md#evidence-and-scope)
(records appended to one open frame per connection); its six cases were run on
2026-10-24 (`proofs/tla/RUNS.tsv`), and its abstractions from the code are
listed in `proofs/tla/README-RNOutgoingBuild.md`.
`RNMessageViews` models internal view ownership; the public receive API has no
release call. Check each model's abstraction and recorded inputs at the source
revision under review.

| Concern | Main modules | Required property |
| --- | --- | --- |
| lossy link and fault injection | `Link`, `RNSimFaults` | the configured loss pattern, fairness assumptions and deterministic fault injection (crash, restart, clock skew) are explicit |
| client handshake | `ClientConnection`, `ClientSession`, `RNClientRace`, `RNClientDelivery` | monotonic states, bounded deadlines, family-race socket ownership, payload batch clearing across connection end, secret cleanup and reconnect reset |
| client transport ladder | `RNClientLadder` | candidates and families tried in order, an IPv6 refusal falls back to the token's IPv4 server on the same candidate, exhaustion only when no named server is reachable, out of memory fatal on either family |
| server handshake and security tables | `ServerConnection`, `ReceiveOrder`, `RNSecurityTables`, `RNFilterEpochs`, `RNServerMaintenanceSweep` | ordered receive rungs, prefilter epoch rotation, cookie gate, token/address binding, slot admission, security-table eviction and bounded maintenance sweeps |
| io_uring server driver, reap and send phases | `RNUringDriver`, `RNUringReapLayout`, `RNUringSendPhases` | no slot reused while the kernel owns it; the fill bank within its room; no call ends with a send completion behind a receive held for room; a multi-slot send run retires all its slots under one completion and `send_pending` counts in-flight sends, not slots; a send reaped one call late never leaves the next update without slots |
| replay and ACKs | `ReplayWindow`, `RNSentHistory`, `SessionAcks`, `SessionSendSequence`, `RNWrittenRing` | replay refusal with a reversed witness for each safety rule, honest ACK ranges, session send sequence bounds, written-bit ring validity and bounded retained history; `SessionAcks` liveness rests on the per-link delivery assumptions `[]<>Flush => []<>RecvData` and `[]<>SendAck => []<>RecvAck` (the current block), with `MCSessionAcksBrokenLiveness.cfg` failing `OpenedSurfacesOrLaps` as the reversed witness |
| channel delivery and flows | `RNChannel`, `RNChannelTypes`, `RNEndpoint`, `RNFlows` | reliable exactly once, ordered in order, sequenced newest, unreliable at most once, end-to-end CONFIG announcement and server flow fan-out; `RNChannel` liveness rests on the delivery assumption `[]<>SendAckThrough => []<>ReceiveUsefulAck`, with `MCRNChannelBrokenDropOnRetransmit.cfg` and `MCRNChannelBrokenChunkBlocksSmall.cfg` failing `EveryOrderedMessageDelivered` and `NoHeadOfLineDeadlock` as the reversed witnesses |
| message blocks | `RNBlocks` | exact reconstruction and bounded slots |
| send store and descriptor pool | `RNSendStore`, `RNLazyPool`, `RNSendStoreLayout`, `RNSendSpans`, `RNConnectionMemory` | no chunk or descriptor handed out while live, exact free counts, free stack and span ring, a refusal or connection kick only when exhausted, no read of never-taken memory, live generations never 0 |
| receipts | `RNReceipts` | at most one terminal receipt for a receipt-bearing send |
| sent map and receipt rings | `RNSentMap`, `RNSentHistory`, `RNSentRingLayout` | no live sent-packet entry or reference overwritten, exact reference accounting, every reported receipt the event written at its index, serials never 0 |
| unreliable queue | `RNIndexRing`, `RNIndexRingArray`, `RNChannelQueues`, `RNUnreliableRingLayout` | the live entries from the head are the queue in push order across ticket wraps, the head always live, a walk returns each queued record at most once and in order from the head as it stood when the walk began |
| resend FIFO | `RNResendGenerations`, `RNResendTiming`, `RNResendFifoLayout` | the array's links, heads, tails, flag bits and count are exactly one sequence per class; each class's sent mark is its first number on the wire (`MarkMatch`) and its numbers on the wire are in order of last send (`Ordered`); with a packet per due record each due record is sent in the update it falls due, and a refused resend is planned as a retransmit by the next call (`RetryPlanned`) |
| round-trip filter | `RNRttFilter` | no sample from a packet whose ack waited out an owed silence, a sample refused only after an owed silence, and a sparse sender's estimate rises when its path slows; no outage poisons the estimate: the first ack of a connection after an owed outage and a copy of an already acknowledged number give no sample (v1.1.1 sampled both, the reversed witnesses) |
| server send loop and budgets | `RNServerSendFair`, `RNOutgoingBuild`, `RNSendRotation`, `RNScheduler`, `RNSendBudget` | a frame is started only for a payload that will be written: the due test agrees with the walk (a slot in probe hold starts a frame only for its probe) and a budget refusal is decided before a frame, so no unit of the shared capacity goes to an aborted frame (`NoUnitWasted`, `OtherClientUnaffected`, `DarkTakesOnlyItsProbes`); every slot with writable work served within ceil(connected / K) flushes; a flush is rounds over the last round's writers in the first round's order, the cursor as a walk over every connected slot would leave it, at most N + frames visits (`SameOrder`, `OnlyLastWriters`, `EndedWritesNothing`, `CursorAsFullWalk`, `VisitsBounded`) |
| server receive allowance | `RNReceiveAllowance` | a client sending within the setting, delivered up to its timeout late, is never refused, its acks and keep-alives included; one slot's refusals never change another's allowance; a time argument that steps back mints nothing; the ack credit pays for no more acks than the server sent eliciting packets |
| endpoint reset and lifecycle | `RNSlots`, `ClientSession`, `RNDestroyWipe`, `RNAllocatorInstall`, `RNCryptoSource`, `RNUpdateClock` | no state or payload crosses a connection generation, secret wipe on destroy, mutex-serialized allocator installation with lock-free allocation/free hot path, crypto source lifecycle, monotonic clamped update clock |
| client I/O | `RNBufferHandoff`, `RNBufferHandoffBase`, `RNClientIo`, `RNClientIoDecrypt`, `RNClientIoShutdown`, `RNClientReceiveQuit`, `RNClientLinkStop`, `RNClientGroCarry` | exclusive buffer ownership, index-only exchanges under a mutex the game thread never waits on, replay before admission, clean worker quit/stop and GRO carry |
| receive window layout | `RNWindow`, `RNWindowLayout`, `RNRing`, `RNHeldOwnership`, `RNSequencedSlot` | each number accepted once and never at or past R + W, the bitmap equal to the received numbers R has not passed, no live descriptor overwritten; `RNSequencedSlot` models the code-4 receive dispatch on the descriptor slots and checks that an honest sender never reaches SLOT_COLLISION for one two-fragment block superseded by a later ordered epoch, with W=4 and W=8 controls and a W=8 reversed accepted-span witness |
| sequenced hold pool | `RNSequencedHoldPool` | no slot returned twice or pushed past the free stack, every slot in exactly one place, no delivered payload rewritten under its view |
| message receive views | `RNSlots`, `RNMessageViews`, `RNPacketLease` | held counts exact, a release always finds the view or packet lease it names, every delivery of an update handed out |
| channel queues layout | `RNChannelQueues`, `RNChannelQueuesLayout` | the links walk each FIFO and the ready list exactly, no link written into a descriptor outside its FIFO, every channel with work takes turns |
| ordering index width | `RNOrderingWrap` | every comparison of the 24-bit wire ordering with the 32-bit read index made modulo 2^24; a full delivery table defers a packet rather than exhausting the region |
| sequencing index width | `RNSequencingWrap` | every sequenced compare of a 24-bit ordering or sequencing index made modulo 2^24 and stale at or past the half range: no newer state discarded, no record exactly half the range behind delivered, held or dropped uncounted |
| replay window layout | `ReplayWindow`, `RNReplayLayout` | the ring bits equal the received sequences of the retained range after every advance, its word-level clear split at the ring end |
| ack block builder layout | `RNAckBuildLayout` | the descending word pass builds the bit-by-bit walk's block, never more ranges than the capacity |
| acknowledged-run walk layout | `RNAckWalkLayout` | the masked popcount and descending walk over the sent ring's words visit exactly the named unacknowledged sequences, newest first; the lowest-acked word skip finds the lowest |
| receive statistics blocks layout | `RNServerStats`, `RNReceivedBlocksLayout` | each generation-tagged summary exact, the cached left, middle and right pieces exact, the older-half publication equal to a record-by-record read |
| rtt minimum tree layout | `RNRttTreeLayout` | every tree node the minimum of its children, the root the minimum positive sample |
| protocol tables' free stacks | `RNOpenAddress`, `RNFreeStackLayout` | each stack holds exactly the slots not live, each once; a map generation never reissued and never 0 |
| wire sequence reconstruction | `AckBlockWrap`, `RNReconstructLayout` | the three-candidate arithmetic chooses the nearest candidate, the larger on a tie |
| transport bank queues | `RNPortableRx`, `RNPortableTx`, `RNGsoHandoff`, `RNGsoRecvFill`, `RNGsoRecvWalk`, `RNBufferHandoff`, `RNRioWorker`, `RNClientIocp`, `RNBankFifoLayout`, `RNMsgxPause` | every bank in exactly one place read through the free and ready arrays, the ready ring in publish order, no position outside an array, for FIFO and stack free queues, both initializers and the iocp client's ring of num_receives, and no GSO receive queue starved by the queues before it |
`RNRioDriver` describes the earlier caller-driven two-phase RIO design; its
recorded results are historical, and the shipped worker is modeled by
`RNRioWorker`.
| RIO send slots | `RNRioWorker`, `RNRioSendSlotLayout` | a reservation returns only a free slot at the counter's own width, the send_ready ring the handed phases in order |
| IOCP server queues and send leases | `RNIocpRefQueue`, `RNIocpRetryStop`, `RNIocpRunRing`, `RNIocpSendLease` | bounded MPMC reference queue with a push a stalled pop refuses kept and retried, stop-ticketed receive retry queue, USO run-ticket ring and generation-tagged send operation leases whose release push a stalled pop refuses is retried |
| client I/O compaction | `RNClientIoDecrypt`, `RNClientIoCompactLayout` | every record names the slot holding its datagram after the in-place compaction, records dense from slot 0 |
| client send cursor | `RNClientSendCursorLayout` | every live slot of a ring is sent at most once, and exactly once on release, on each family that was active for it, in ring order, and never after that ring is released |
| kernel replay pending buffer | `RNXdpReplay`, `RNXdpReplayMarksLayout` | no write past the buffer, the arrays' prefixes exactly the pending forgets and marks in order across each in-place compaction, the kept prefix naming each key once |
| reply MAC table and XDP rings | `RNXdpReplay`, `RNXdpReplyMacsLayout`, `RNXdpGatewayRefresh`, `RNXdpReceiveRing` | every lookup returns the address's learned MAC, else its answered one, one slot per address within the probe window with no empty slot before it, evictions of the oldest entry only, bounded gateway refresh and AF_XDP receive frame ownership |
| XDP TX drain at stop | `RNXdpSendDrain` | a queue whose NIC still takes TX descriptors is never given up on while it holds frames; every stop finishes, a NIC that never takes another descriptor included |
| deferred disconnect bursts | `ServerSession`, `RNDisconnectBurstRing`, `ServerStopSpread` | no waiting burst overwritten, seals only for waiting bursts, one packet per still-waiting burst per pass when a burst is abandoned and a complete entry is passed over, sealed counts of bursts still waiting never rising from the head, a burst that was not abandoned leaving with exactly its packets sealed, a stop step whose transport reports no send room and is idle (and still has no room when read again) abandoning every waiting burst, each counted once, rather than waiting forever |
| replay walk slot records | `RNXdpReplay`, `RNServerReplaySlots` | a forget owed for a slot's old address never lost across slot reuse; a forget the sink refused never deletes a later session's kernel entry: a walk that left a refused forget owes the next forget pass a scan of the other slots, and two records name one address only then (reversed witnesses `BrokenStaleForget` and `BrokenNoOweOnRefusal`) |
| loopback bind table | `LoopbackHub`, `LoopbackHubLayout`, `LoopbackQueue`, `RNLoopbackBindLayout` | one endpoint per family and port, at most one endpoint accepting any IPv4 destination (a dual-stack IPv6 wildcard shares its port with no IPv4 endpoint), the one-lookup destination equal to a walk of the live records, ephemeral ports in range and free in both families, a port 0 bind refused only when every ephemeral port is held |
| client slots free stack | `ServerConnection`, `RNOpenAddress`, `RNClientSlotsLayout` | each free slot on the stack exactly once, no slot taken while held, a handle releases only its own tenure, a slot never holds an owner generation twice |
| link simulator store | `Link`, `RNSimulatorStoreLayout` | the bitmap walk chooses the slot the full scan chooses, a send takes the lowest free slot, every slot below the free hint occupied, summary bits exactly the nonempty words, the shard bound below every delivery time, a stale handle never retires a live packet |
| server payload ring | `RNDriverSendStaging`, `RNServerPayloadRingLayout` | no slot at or past the send capacity handed out or read, each flush sends exactly the slots finished since the last, a slot held by one thread at its own frame |
| live fleet and split-load barriers | `RNLiveFleetRun`, `SplitStart` | live fleet lifecycle phases and split-load start barrier synchronization |

ReplayWindow's two gate invariants (`NoAdvanceBeforeConnect` and
`NoOpenBeforeConnect`), RNWrittenRing's `NoUnwrittenRead`, and RNOpenAddress's
clear-first `EraseEnds` hold by construction. The gated receive never opens or
advances a window that has no slot, a read that checks the written bit never
takes an unwritten record's bytes, and the clear-first erase increments its
scan only while `scanned < N`. Those invariants stay in their modules.

Model constants must stay small enough for exhaustive checking while retaining
the production relationship being proved. A model-check result is evidence for
its stated abstraction; unit and functional gates still verify the C++ seam.
Deliberately broken configurations remain negative controls and are named as
such in the run inventory.
| packet builder plan layout | `RNScheduler`, `RNBuilderPlanLayout` | every radix scatter writes a distinct slot, the plan holds each writable item once in class, channel and caller order, the groups tile it one per class and channel with each class range exactly its groups, every suffix level the least priority to its group's end, and across a refused packet's cursor undo each channel's items written or still planned in caller order, none lost and none twice |
| server lifecycle and session slots | `ServerSession` | create/start/stop/destroy, clients reusing slots through the one per-slot reset, kept reasons and the per-run token history, and the message layer's own end of a CONNECTED slot of a RUNNING server (`MessageEnd`) with CONFIG_MISMATCH, PROTOCOL_VIOLATION, RELIABLE_TIMED_OUT, MESSAGE_MEMORY_EXHAUSTED and MESSAGE_NOT_RECEIVED; `MCServerSessionBrokenMessageEnd.cfg` reverses its connected-slot rule; the multi-client invariants are checked by `MCServerSessionTwoClients.cfg` (two clients connected at once, reached per `MCServerSessionTwoClientsReachable.cfg`), not by the one-token `MCServerSession.cfg` |
| IOCP server queues and send leases | `RNIocpRefQueue`, `RNIocpRetryStop`, `RNIocpRunRing`, `RNIocpSendLease` | bounded MPMC reference queue with a push a stalled pop refuses kept and retried, stop-ticketed receive retry queue, USO run-ticket ring and generation-tagged send operation leases |
| reply MAC table and XDP rings | `RNXdpReplay`, `RNXdpReplyMacsLayout`, `RNXdpGatewayRefresh`, `RNXdpReceiveRing` | every lookup returns the address's learned MAC, else its answered one, one slot per address within the probe window with no empty slot before it, evictions of the oldest entry only, bounded gateway refresh on the platform monotonic clock through the known gateway's neighbour entry, and AF_XDP receive frame ownership |
