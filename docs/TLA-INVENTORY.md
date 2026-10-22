# TLA+ inventory

The `tla/` directory contains executable specifications for RocketNet's
bounded protocol and message machines. `tla/RUNS.tsv` is the
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
| receipts | `RNReceipts` | at most one terminal receipt for a receipt-bearing send |
| endpoint reset | `RNSlots`, `ClientSession` | no state or payload crosses a connection generation |
| client I/O | `RNBufferHandoff`, `RNClientIo`, `RNClientIoDecrypt` | exclusive buffer ownership, index-only exchanges under a mutex the game thread never waits on, replay before admission and one retirement |
| handoff | `SendHandoff` | bounded queues, reopen after refusal and no lost owned frame |

Model constants must stay small enough for exhaustive checking while retaining
the production relationship being proved. A model-check result is evidence for
its stated abstraction; unit and functional gates still verify the C++ seam.
Deliberately broken configurations remain negative controls and are named as
such in the run inventory.
