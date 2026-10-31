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
| sent map and receipt rings | `RNSentMap`, `RNSentHistory`, `RNSentRingLayout` | no live sent-packet entry or reference overwritten, exact reference accounting, every reported receipt the event written at its index, serials never 0 |
| resend FIFO | `RNChannelQueues`, `RNResendTiming`, `RNResendFifoLayout` | the array's links, heads, tails, flag bits and count are exactly one sequence per class |
| server send loop | `RNServerSendFair`, `RNOutgoingBuild`, `RNSendRotation` | capacity spent only on written frames except one abort a flush for a refused budget, a flush ends spent or with nothing writable, every connection with work served within N flushes |
| endpoint reset | `RNSlots`, `ClientSession` | no state or payload crosses a connection generation |
| client I/O | `RNBufferHandoff`, `RNClientIo`, `RNClientIoDecrypt` | exclusive buffer ownership, index-only exchanges under a mutex the game thread never waits on, replay before admission and one retirement |

Model constants must stay small enough for exhaustive checking while retaining
the production relationship being proved. A model-check result is evidence for
its stated abstraction; unit and functional gates still verify the C++ seam.
Deliberately broken configurations remain negative controls and are named as
such in the run inventory.
