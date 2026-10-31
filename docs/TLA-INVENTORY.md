# TLA+ inventory

The `proofs/tla/` directory contains executable specifications for RocketNet's
bounded protocol and message machines. `proofs/tla/RUNS.tsv` is the
maintained run inventory; the repository TLA checker validates its schema,
inputs and recorded result for each required group.

| Concern | Main modules | Required property |
| --- | --- | --- |
| lossy link | `Link` | the configured loss pattern and fairness assumptions are explicit |
| client handshake | `ClientConnection`, `ClientSession` | monotonic states, bounded deadlines, secret cleanup and reconnect reset |
| server handshake | `ServerConnection` | cookie gate, token/address binding, slot admission and bounded work |
| replay and ACKs | `ReplayWindow`, `RNSentHistory` | replay refusal, honest ACK ranges and bounded retained history |
| channel delivery | `RNChannel`, `RNChannelTypes` | reliable exactly once, ordered in order, sequenced newest and unreliable at most once |
| fragmentation | `RNBlocks`, `FragmentsAndAcks` | exact reconstruction, bounded slots and ACK coupling |
| send store and descriptor pool | `RNSendStore`, `RNLazyPool`, `RNSendStoreLayout` | no chunk or descriptor handed out while live, exact free counts and free stack, a refusal only when nothing is free, no read of never-taken memory, live generations never 0 |
| receipts | `RNReceipts` | at most one terminal receipt for a receipt-bearing send |
| endpoint reset | `RNSlots`, `ClientSession` | no state or payload crosses a connection generation |
| client I/O | `RNBufferHandoff`, `RNClientIo`, `RNClientIoDecrypt` | exclusive buffer ownership, index-only exchanges under a mutex the game thread never waits on, replay before admission and one retirement |
| receive window layout | `RNWindow`, `RNWindowLayout` | each number accepted once and never at or past R + W, the bitmap equal to the received numbers R has not passed, no live descriptor overwritten |
| sequenced hold pool | `RNSequencedHoldPool` | no slot returned twice or pushed past the free stack, every slot in exactly one place, no delivered payload rewritten under its view |
| message receive views | `RNSlots`, `RNMessageViews` | held counts exact, a release always finds the view it names, every delivery of an update handed out |
| channel queues layout | `RNChannelQueues`, `RNChannelQueuesLayout` | the links walk each FIFO and the ready list exactly, no link written into a descriptor outside its FIFO, every channel with work takes turns |
| ordering index width | `RNOrderingWrap` | every comparison of the 24-bit wire ordering with the 32-bit read index made modulo 2^24; a full delivery table defers a packet rather than exhausting the region |

Model constants must stay small enough for exhaustive checking while retaining
the production relationship being proved. A model-check result is evidence for
its stated abstraction; unit and functional gates still verify the C++ seam.
Deliberately broken configurations remain negative controls and are named as
such in the run inventory.
