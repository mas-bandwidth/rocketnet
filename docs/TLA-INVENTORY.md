# TLA+ inventory

The `proofs/tla/` directory contains executable specifications for RocketNet's
bounded protocol and message machines. `proofs/tla/RUNS.tsv` is the
maintained run inventory; the repository TLA checker validates its schema,
inputs and recorded result for each required group.

This is a navigation inventory, not a claim that every model matches every
current implementation detail. In particular, `RNOutgoingBuild` describes the
earlier one-message-per-frame design; its recorded results do not qualify the
current [unreliable aggregation path](OUTGOING-BUILD.md#evidence-and-scope).
`RNMessageViews` models internal view ownership; the public receive API has no
release call. Check each model's abstraction and recorded inputs at the source
revision under review.

| Concern | Main modules | Required property |
| --- | --- | --- |
| lossy link | `Link` | the configured loss pattern and fairness assumptions are explicit |
| client handshake | `ClientConnection`, `ClientSession` | monotonic states, bounded deadlines, secret cleanup and reconnect reset |
| client transport ladder | `RNClientLadder` | candidates and families tried in order, an IPv6 refusal falls back to the token's IPv4 server on the same candidate, exhaustion only when no named server is reachable, out of memory fatal on either family |
| server handshake | `ServerConnection` | cookie gate, token/address binding, slot admission and bounded work |
| io_uring server driver, reap and send phases | `RNUringDriver`, `RNUringReapLayout`, `RNUringSendPhases` | no slot reused while the kernel owns it; the fill bank within its room; no call ends with a send completion behind a receive held for room; a send reaped one call late never leaves the next update without slots |
| replay and ACKs | `ReplayWindow`, `RNSentHistory` | replay refusal, honest ACK ranges and bounded retained history |
| channel delivery | `RNChannel`, `RNChannelTypes` | reliable exactly once, ordered in order, sequenced newest and unreliable at most once |
| message blocks | `RNBlocks` | exact reconstruction and bounded slots |
| send store and descriptor pool | `RNSendStore`, `RNLazyPool`, `RNSendStoreLayout` | no chunk or descriptor handed out while live, exact free counts and free stack, a refusal only when nothing is free, no read of never-taken memory, live generations never 0 |
| receipts | `RNReceipts` | at most one terminal receipt for a receipt-bearing send |
| sent map and receipt rings | `RNSentMap`, `RNSentHistory`, `RNSentRingLayout` | no live sent-packet entry or reference overwritten, exact reference accounting, every reported receipt the event written at its index, serials never 0 |
| unreliable queue | `RNIndexRing`, `RNChannelQueues`, `RNUnreliableRingLayout` | the live entries from the head are the queue in push order across ticket wraps, the head always live, a walk returns each queued record at most once and in order |
| resend FIFO | `RNResendGenerations`, `RNResendTiming`, `RNResendFifoLayout` | the array's links, heads, tails, flag bits and count are exactly one sequence per class |
| server send loop | `RNServerSendFair`, `RNOutgoingBuild`, `RNSendRotation` | capacity spent only on written frames except one abort a flush for a refused budget, a flush ends spent or with nothing writable, every connection with work served within N flushes |
| endpoint reset | `RNSlots`, `ClientSession` | no state or payload crosses a connection generation |
| client I/O | `RNBufferHandoff`, `RNClientIo`, `RNClientIoDecrypt` | exclusive buffer ownership, index-only exchanges under a mutex the game thread never waits on, replay before admission and one retirement |
| receive window layout | `RNWindow`, `RNWindowLayout` | each number accepted once and never at or past R + W, the bitmap equal to the received numbers R has not passed, no live descriptor overwritten |
| sequenced hold pool | `RNSequencedHoldPool` | no slot returned twice or pushed past the free stack, every slot in exactly one place, no delivered payload rewritten under its view |
| message receive views | `RNSlots`, `RNMessageViews` | held counts exact, a release always finds the view it names, every delivery of an update handed out |
| channel queues layout | `RNChannelQueues`, `RNChannelQueuesLayout` | the links walk each FIFO and the ready list exactly, no link written into a descriptor outside its FIFO, every channel with work takes turns |
| ordering index width | `RNOrderingWrap` | every comparison of the 24-bit wire ordering with the 32-bit read index made modulo 2^24; a full delivery table defers a packet rather than exhausting the region |
| replay window layout | `ReplayWindow`, `RNReplayLayout` | the ring bits equal the received sequences of the retained range after every advance, its word-level clear split at the ring end |
| ack block builder layout | `RNAckBuildLayout` | the descending word pass builds the bit-by-bit walk's block, never more ranges than the capacity |
| acknowledged-run walk layout | `RNAckWalkLayout` | the masked popcount and descending walk over the sent ring's words visit exactly the named unacknowledged sequences, newest first; the lowest-acked word skip finds the lowest |
| receive statistics blocks layout | `RNServerStats`, `RNReceivedBlocksLayout` | each generation-tagged summary exact, the cached left, middle and right pieces exact, the older-half publication equal to a record-by-record read |
| rtt minimum tree layout | `RNRttTreeLayout` | every tree node the minimum of its children, the root the minimum positive sample |
| protocol tables' free stacks | `RNOpenAddress`, `RNFreeStackLayout` | each stack holds exactly the slots not live, each once; a map generation never reissued and never 0 |
| wire sequence reconstruction | `AckBlockWrap`, `RNReconstructLayout` | the three-candidate arithmetic chooses the nearest candidate, the larger on a tie |
| transport bank queues | `RNPortableRx`, `RNPortableTx`, `RNGsoHandoff`, `RNBufferHandoff`, `RNRioWorker`, `RNClientIocp`, `RNBankFifoLayout` | every bank in exactly one place read through the free and ready arrays, the ready ring in publish order, no position outside an array, for FIFO and stack free queues, both initializers and the iocp client's ring of num_receives |
| RIO send slots | `RNRioDriver`, `RNRioWorker`, `RNRioSendSlotLayout` | a reservation returns only a free slot at the counter's own width, the send_ready ring the handed phases in order |
| client I/O compaction | `RNClientIoDecrypt`, `RNClientIoCompactLayout` | every record names the slot holding its datagram after the in-place compaction, records dense from slot 0 |
| kernel replay pending buffer | `RNXdpReplay`, `RNXdpReplayMarksLayout` | no write past the buffer, the arrays' prefixes exactly the pending forgets and marks in order across each in-place compaction, the kept prefix naming each key once |
| reply MAC table | `RNXdpReplay`, `RNXdpReplyMacsLayout` | every lookup returns the address's learned MAC, else its answered one, one slot per address within the probe window with no empty slot before it, evictions of the oldest entry only |
| deferred disconnect bursts | `ServerSession`, `RNDisconnectBurstRing` | no waiting burst overwritten, seals only for waiting bursts, one packet per waiting burst per pass, each burst leaving with exactly its packets sealed |
| replay walk slot records | `RNXdpReplay`, `RNServerReplaySlots` | a forget owed for a slot's old address never lost across slot reuse; a forget the sink refused can delete a later session's kernel entry (a recorded finding) |
| loopback bind table | `LoopbackHub`, `RNLoopbackBindLayout` | one endpoint per family and port, the one-lookup destination equal to a walk of the live records, ephemeral ports in range and free in both families |
| client slots free stack | `ServerConnection`, `RNOpenAddress`, `RNClientSlotsLayout` | each free slot on the stack exactly once, no slot taken while held, a handle releases only its own tenure, a slot never holds an owner generation twice |
| link simulator store | `Link`, `RNSimulatorStoreLayout` | the bitmap walk chooses the slot the full scan chooses, a send takes the lowest free slot, every slot below the free hint occupied, summary bits exactly the nonempty words, the shard bound below every delivery time, a stale handle never retires a live packet |
| server payload ring | `RNDriverSendStaging`, `RNServerPayloadRingLayout` | no slot at or past the send capacity handed out or read, each flush sends exactly the slots finished since the last, a slot held by one thread at its own frame |

Model constants must stay small enough for exhaustive checking while retaining
the production relationship being proved. A model-check result is evidence for
its stated abstraction; unit and functional gates still verify the C++ seam.
Deliberately broken configurations remain negative controls and are named as
such in the run inventory.
