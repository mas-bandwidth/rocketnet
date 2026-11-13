# RNMsgxReceiveFill

The macOS batched server transport's receive worker over its three banks,
`sdk/rocketnet_msgx_receive_fill.h`, driven by
`rocketnet_server_msgx_receive_worker` and claimed by
`rocketnet_server_msgx_receive_packets` (`sdk/rocketnet_server_msgx_apple.cpp`).

## Why

Review E3 (found in review by Emma): the worker read a whole `recvmsg_x` batch
whatever the room in its bank, and a full bank that main had not yet taken
dropped what it read, counted as a full bank. The kernel's socket buffers
were never used as the queue, against the rule `RNGsoHandoff.tla` states for
gso (`RxFill`, `RxPublish`) and HoldOnFull. The bank was also 8192 against
an update's receive budget of 16000 at 1000 clients, so 64 sockets of 150
datagrams in one update dropped 1408.

## The rule

A datagram is read only into room (`Want`). A full bank is handed to main at
once when main has taken the last one (`PublishFull`); otherwise the drain
holds (`HoldFull`), the round ends, every socket keeps what it holds, and the
worker sleeps (`Sleep`) before it polls again. The kernel's receive buffer is
the queue: a datagram is dropped only by the kernel, on arrival to a full
buffer (`KernelDrop`).

## Actions

| Action | Code |
|---|---|
| `Poll` | the worker's `kevent` and round over the ready sockets |
| `PublishFull`, `HoldFull`, `BudgetSpent`, `Read(n)`, `ReadEmpty`, `ReadWeather`, `ReadFailed` | `rocketnet_msgx_receive_drain_step`, one per result |
| `RoundPublish`, `RoundKeep` | the round's publish through `rocketnet_msgx_receive_publish` |
| `Sleep` | `ROCKETNET_MSGX_RECV_HOLD_SECONDS` after a held round |
| `Consume`, `Claim` | main's consume of its bank, then `rocketnet_msgx_receive_claim` |
| `Arrive`, `KernelDrop`, `Error` | environment: the kernel and the network |

`ReadWeather` is an ICMP error the kernel queued for a connected client
(ECONNREFUSED, EHOSTUNREACH, ENETUNREACH, EHOSTDOWN, ENETDOWN), EINTR or
ENOBUFS: cleared by the read that reports it, read past at one read of the
budget (review E5, the platform's receive weather). `ReadFailed` is a dead
socket (ENOTCONN, EPIPE, EDESTADDRREQ, reported to the table) or any other
error: counted, the socket left for the round.

The model counts datagrams rather than naming them: which datagram is where
does not bear on the properties. The worker loop also hands over a bank
that holds only counts of reads and refusals (no datagram), so
`RoundPublish` is allowed with an empty bank; `RoundKeep` never keeps a bank
of datagrams main could take.

The drain step, the publish and the claim are library code the trace walk
calls. The worker loop around them (the round over the ready sockets, the
held flag, the round's publish, the sleep) is about ten lines of
`rocketnet_server_msgx_receive_worker` that the walk follows by hand, as are
main's consume and claim around `rocketnet_msgx_receive_claim`.

## Properties

- `HandOK`: the banks are the worker's, main's, and one ready or free.
- `NoFullDrop`: nothing read is dropped for want of room.
- `Conserved`: every datagram is in a socket, a bank, main's hands, or a
  kernel drop.
- `HeldOnlyWhenFull`: a held worker's bank is full. Main may take the ready
  bank while the worker sleeps; the next round hands the full bank over.
- `HoldOnlyWhenNowhere` (an action property): the drain holds only with the
  bank full and main not yet done with the last, so the worker never stalls
  with room or with a bank it could hand over.
- `AllConsumed` (under `FairSpec`): every datagram that arrives is main's or
  the kernel's drop in the end.

## Configurations

| Config | Instance | Expected |
|---|---|---|
| `MCRNMsgxReceiveFill.cfg` | 2 sockets, banks of 3, batch 2, budget 3, buffers of 3, 7 arrivals, 2 errors | pass |
| `MCRNMsgxReceiveFillLive.cfg` | the same, 5 arrivals, 1 error, `FairSpec` | pass, `AllConsumed` |
| `MCRNMsgxReceiveFillBrokenReadPastRoom.cfg` | `ReadPastRoom`: a whole batch whatever the room, the code before E3 | `NoFullDrop` fails |
| `MCRNMsgxReceiveFillBrokenPublishOverReady.cfg` | `PublishOverReady`: a full bank handed over a ready one | `Conserved` fails |
| `MCRNMsgxReceiveFillBrokenHoldWithoutPublish.cfg` | `HoldWithoutPublish`: held when it could hand over | `HoldOnlyWhenNowhere` fails |

## Trace check

`TraceRNMsgxReceiveFill.tla` is the trace check of this module (layer
`msgxreceivefill`, `tools/tla_trace_check.py`).
`tests/tla_trace_msgx_receive_fill.cpp` drives the real
`rocketnet_msgx_receive_drain_step`, `rocketnet_msgx_receive_publish` and
`rocketnet_msgx_receive_claim` over a fake kernel of three sockets with banks
of six and a batch of four, so reads meet a room below a batch. The plant
(`ROCKETNET_MSGX_RECEIVE_FILL_PLANT`, `rocketnet_msgx_receive_want` in
`sdk/rocketnet_msgx_receive_fill.cpp` asking for a whole batch) must be
rejected at the first read that takes past the room. The reach predicates
`ShortRoom` and `Held` must each be reached by a clean trace.
`MCTraceRNMsgxReceiveFillSelf.cfg` and
`MCTraceRNMsgxReceiveFillSelfBrokenReadPastRoom.cfg` are the checker's own
controls. The unit tests (`tests/test_unit_msgx_receive_fill.cpp`) run the
same functions over the same kind of fake, with the review's 64 sockets of
150 datagrams.
