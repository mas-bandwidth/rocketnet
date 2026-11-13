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

A datagram is read only into room, and a read asks for no more than the
socket's budget for the round leaves (`Want`). A full bank is handed to main at
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
| `MainStop`, `Exit` | `rocketnet_server_msgx_stop` (main's tail counted, then `rocketnet_msgx_receive_stop`), and the worker's exit at the top of its loop, which counts the bank it fills and a ready bank main has not claimed |
| `Done` | the end once the worker has exited and the arrivals bound is spent, a stutter |
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

## The stop (R69)

At `rocketnet_server_msgx_stop` main counts the tail of its own bank stopped,
and the server goes on calling the transport's receive until the transport
reports stopped, that is until the worker has exited. Before R69 one of
those calls could claim a bank the worker published after the stop: main
then read part of it in that update, the server saw the transport stopped
and called no more, and the rest was neither processed nor counted (the
worker's exit counts only the bank it fills and a ready bank). Now the stop
sets the hand's `stopped` flag and `rocketnet_msgx_receive_claim` refuses
once it is set, so a ready bank stays the worker's and is counted
`RECEIVE_DROP_STOPPED` at its exit. `ClaimAfterStop` is the code before.

## Properties

- `HandOK`: the banks are the worker's, main's, and one ready or free.
- `NoFullDrop`: nothing read is dropped for want of room.
- `BudgetKept`: a socket's reads in a round never pass its budget: a read asks
  for no more than the budget leaves (Stella's review of 4119919c7, note 2;
  before, a read asked for the whole batch and could pass the budget by up
  to `Batch - 1`).
- `Conserved`: every datagram is in a socket, a bank, main's hands, or a
  kernel drop.
- `HeldOnlyWhenFull`: a held worker's bank is full. Main may take the ready
  bank while the worker sleeps; the next round hands the full bank over.
- `HoldOnlyWhenNowhere` (an action property): the drain holds only with the
  bank full and main not yet done with the last, so the worker never stalls
  with room or with a bank it could hand over.
- `AllConsumed` (under `FairSpec`): every datagram that arrives is main's or
  the kernel's drop in the end, unless main stops first.
- `StopCounted`: once the worker has exited no bank holds a datagram, so
  every datagram read was consumed by main or counted stopped (R69).
- `StopEnds` (under `FairSpec`): a stop ends the worker.

## Configurations

| Config | Instance | Expected |
|---|---|---|
| `MCRNMsgxReceiveFill.cfg` | 2 sockets, banks of 3, batch 2, budget 3, buffers of 3, 7 arrivals, 2 errors | pass |
| `MCRNMsgxReceiveFillLive.cfg` | the same, 5 arrivals, 1 error, `FairSpec` | pass, `AllConsumed`, `StopEnds` |
| `MCRNMsgxReceiveFillBrokenReadPastRoom.cfg` | `ReadPastRoom`: a whole batch whatever the room, the code before E3 | `NoFullDrop` fails |
| `MCRNMsgxReceiveFillBrokenPublishOverReady.cfg` | `PublishOverReady`: a full bank handed over a ready one | `Conserved` fails |
| `MCRNMsgxReceiveFillBrokenHoldWithoutPublish.cfg` | `HoldWithoutPublish`: held when it could hand over | `HoldOnlyWhenNowhere` fails |
| `MCRNMsgxReceiveFillBrokenClaimAfterStop.cfg` | `ClaimAfterStop`: main claims a ready bank after its stop, the code before R69 | `StopCounted` fails |
| `MCRNMsgxReceiveFillBrokenReadPastBudget.cfg` | `ReadPastBudget`: a read asks for the batch whatever the budget leaves, the drain before note 2 | `BudgetKept` fails |

## Trace check

`TraceRNMsgxReceiveFill.tla` is the trace check of this module (layer
`msgxreceivefill`, `tools/tla_trace_check.py`).
`tests/tla_trace_msgx_receive_fill.cpp` drives the real
`rocketnet_msgx_receive_drain_step`, `rocketnet_msgx_receive_publish` and
`rocketnet_msgx_receive_claim` over a fake kernel of three sockets with banks
of six and a batch of four, so reads meet a room below a batch. On even
seeds main stops three quarters of the way through (`rocketnet_msgx_receive_stop`)
and the worker exits at its next poll, so `MainStop` and `Exit` are traced and
a claim after the stop would be rejected. The plant
(`ROCKETNET_MSGX_RECEIVE_FILL_PLANT`, `rocketnet_msgx_receive_want` in
`sdk/rocketnet_msgx_receive_fill.cpp` asking for a whole batch) must be
rejected at the first read that takes past the room. The reach predicates
`ShortRoom`, `Held` and `BudgetCaps` (a read capped by the budget) must each be
reached by a clean trace.
`MCTraceRNMsgxReceiveFillSelf.cfg` and
`MCTraceRNMsgxReceiveFillSelfBrokenReadPastRoom.cfg` are the checker's own
controls. The unit tests (`tests/test_unit_msgx_receive_fill.cpp`) run the
same functions over the same kind of fake, with the review's 64 sockets of
150 datagrams.
