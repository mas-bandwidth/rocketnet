# RNMsgxReceiveFill

The macOS batched server transport's receive worker over its three banks,
`sdk/rocketnet_msgx_receive_fill.h`, driven by
`rocketnet_server_msgx_receive_worker`, and main's receive call over the
shared receive handoff (`sdk/rocketnet_receive_handoff.h`), driven by
`rocketnet_server_msgx_receive_packets` (`sdk/rocketnet_server_msgx_apple.cpp`).

## Why

Review E3 (found in review by Emma): the worker read a whole `recvmsg_x` batch
whatever the room in its bank, and a full bank that main had not yet taken
dropped what it read, counted as a full bank. The kernel's socket buffers
were never used as the queue, against the rule `RNGsoHandoff.tla` states for
gso (`RxFill`, `RxPublish`, at 5a2dbf403) and HoldOnFull. The bank was also
8192 against an update's receive budget of 16000 at 1000 clients, so 64
sockets of 150 datagrams in one update dropped 1408.

The receive latency fix: main read only a bank the worker had published
(the ready-slot rule), so a datagram the worker read after its last publish
waited for main's next update even when it was banked before main's call.
Main now reads the worker's bank up to the count the worker has committed,
as gso does (`RNReceiveHandoff.tla`), and a datagram committed before a call
is returned by that call (`DeliveredByNextCall`).

## The rule

A datagram is read only into room, and a read asks for no more than the
socket's budget for the round leaves (`Want`). A full bank moves on to the
free bank at once when main has started on it (`MoveFull`, a commit);
otherwise the drain holds (`HoldFull`), the round ends, every socket keeps
what it holds, and the worker sleeps (`Sleep`) before it polls again. The
kernel's receive buffer is the queue: a datagram is dropped only by the
kernel, on arrival to a full buffer (`KernelDrop`). A round that read commits
its bank's count under the mutex (`RoundCommit`), so main sees it at its next
call.

## Actions

| Action | Code |
|---|---|
| `Poll` | the worker's `kevent` and round over the ready sockets |
| `MoveFull`, `HoldFull`, `BudgetSpent`, `Read(n)`, `ReadEmpty`, `ReadWeather`, `ReadFailed` | `rocketnet_msgx_receive_drain_step`, one per result; `MoveFull` commits through `rocketnet_server_msgx_commit_receive` |
| `RoundCommit`, `RoundKeep` | the round's end: a round that read commits through `rocketnet_server_msgx_commit_receive` (`rocketnet_receive_handoff_commit`) |
| `Sleep` | `ROCKETNET_MSGX_RECV_HOLD_SECONDS` after a held round |
| `Call(busy, c)` | `rocketnet_msgx_receive_call` from `rocketnet_server_msgx_receive_packets`: a finished tail emptied, a try-lock and `rocketnet_receive_handoff_begin`, then the tail and main's bank up to the committed count within capacity `c` |
| `MainStop`, `Exit` | `rocketnet_server_msgx_stop` (main calls no more), and the worker's exit at the top of its loop |
| `StopCount` | `rocketnet_msgx_receive_stop_count` in `rocketnet_server_msgx_count_stopped`, once main finds the worker exited: the tail's unread, main's bank past its cursor and the worker's bank counted stopped |
| `Done` | the end once the stop is counted and the arrivals bound is spent, a stutter |
| `Arrive`, `KernelDrop`, `Error` | environment: the kernel and the network |

`ReadWeather` is an ICMP error the kernel queued for a connected client
(ECONNREFUSED, EHOSTUNREACH, ENETUNREACH, EHOSTDOWN, ENETDOWN), EINTR or
ENOBUFS: cleared by the read that reports it, read past at one read of the
budget (review E5, the platform's receive weather). `ReadFailed` is a dead
socket (ENOTCONN, EPIPE, EDESTADDRREQ, reported to the table) or any other
error: counted, the socket left for the round.

The model counts datagrams rather than naming them: which datagram is where
does not bear on the properties (`RNReceiveHandoff` carries the order). A
call is one step, as in `RNReceiveHandoff`: what main reads lies below counts
the worker no longer changes.

The drain step, the commit, the call and the stop count are library code the
trace walk calls. The worker loop around them (the round over the ready
sockets, the held flag, the round's commit when it read, the sleep) is about
ten lines of `rocketnet_server_msgx_receive_worker` that the walk follows by
hand, as is the order of the stop, the exit and the count.

## The stop (R69)

At `rocketnet_server_msgx_stop` main stops calling, and the server goes on
asking the transport whether it has stopped until the worker has exited.
Before R69 a call after the stop could take a bank the worker handed over
after it: main read part of it in that update, the server saw the transport
stopped and called no more, and the rest was neither processed nor counted.
Now no call runs after the stop, and once the worker has exited main counts
once what it never returned (`StopCount`, as gso's): the tail's unread, its
own bank past its cursor and the worker's bank when it is another. The worker
exits only at the top of its loop, after its round's commit, so each bank's
count is then its committed count. `StopCountSkipsWorker` is the count that
leaves the worker's bank out once the worker has moved on from main's bank.

The worker's round that reads nothing (kevent reports no ready socket, or
only stale events) ends with no commit; the model's `Poll` visits every
socket, so `RoundKeep` is that round's action but is never taken in the
model, and the trace check lists it as untraced.

## Properties

- `FreeIffShared`, `NoSharedSlot`, `FreeIsEmpty`: the handoff's roles, as in
  `RNReceiveHandoff`.
- `NoFullDrop`: nothing read is dropped for want of room.
- `BudgetKept`: a socket's reads in a round never pass its budget: a read asks
  for no more than the budget leaves (Stella's review of 4119919c7, note 2;
  before, a read asked for the whole batch and could pass the budget by up
  to `Batch - 1`).
- `Conserved`: every datagram is in a socket, a bank, main's hands, a
  kernel drop, or counted stopped.
- `HeldOnlyWhenFull`: a held worker's bank is full.
- `HoldOnlyWhenNowhere` (an action property): the drain holds only with the
  bank full and main not yet started on it, so the worker never stalls with
  room or with a bank it could move on from.
- `DeliveredByNextCall`: a datagram committed before a call that took the
  mutex, found no backlog and was not cut by its capacity is returned by that
  call.
- `AllConsumed` (under `FairSpec`): every datagram that arrives is main's or
  the kernel's drop in the end, unless main stops first. This is a claim about
  the model's finite arrivals with the worker and main fair: `Poll` visits every
  modelled socket each round, so it says nothing about the kernel's readiness
  order or fairness between sockets under sustained load (Stella's review of
  4119919c7, note 3).
- `StopCounted`: once the stop is counted every datagram that arrived is in
  a socket, consumed by main, dropped by the kernel or for want of room, or
  counted stopped: none is left in a bank uncounted (R69).
- `StopEnds` (under `FairSpec`): a stop is counted in the end.

## Configurations

| Config | Instance | Expected |
|---|---|---|
| `MCRNMsgxReceiveFill.cfg` | 2 sockets, banks of 3, batch 2, budget 3, buffers of 3, calls of 1 or 3, 6 arrivals, 1 error | pass |
| `MCRNMsgxReceiveFillLive.cfg` | the same, 5 arrivals, `FairSpec` | pass, `AllConsumed`, `StopEnds` |
| `MCRNMsgxReceiveFillBrokenReadPastRoom.cfg` | `ReadPastRoom`: a whole batch whatever the room, the code before E3 | `NoFullDrop` fails |
| `MCRNMsgxReceiveFillBrokenFreeNotReset.cfg` | `FreeNotReset`: a finished tail freed without emptying its count | `Conserved` fails |
| `MCRNMsgxReceiveFillBrokenHoldWithoutMove.cfg` | `HoldWithoutMove`: held when it could move on | `HoldOnlyWhenNowhere` fails |
| `MCRNMsgxReceiveFillBrokenStopCountSkipsWorker.cfg` | `StopCountSkipsWorker`: the stop leaves out the worker's bank when the worker has moved on from main's | `StopCounted` fails |
| `MCRNMsgxReceiveFillBrokenReadPastBudget.cfg` | `ReadPastBudget`: a read asks for the batch whatever the budget leaves, the drain before note 2 | `BudgetKept` fails |
| `MCRNMsgxReceiveFillBrokenReadOnlyLeft.cfg` | `ReadOnlyLeft`: main reads only a bank the worker has left, the ready-slot rule before the latency fix | `DeliveredByNextCall` fails |

## Trace check

`TraceRNMsgxReceiveFill.tla` is the trace check of this module (layer
`msgxreceivefill`, `tools/tla_trace_check.py`).
`tests/tla_trace_msgx_receive_fill.cpp` drives the real
`rocketnet_msgx_receive_drain_step`, `rocketnet_receive_handoff_commit`,
`rocketnet_msgx_receive_call` and `rocketnet_msgx_receive_stop_count` over a
fake kernel of three sockets with banks of six and a batch of four, so reads
meet a room below a batch, with main's calls sometimes finding the mutex busy
and sometimes cut by a capacity of two. On even seeds main stops three
quarters of the way through, the worker exits at its next poll and main
counts the stop, so `MainStop`, `Exit` and `StopCount` are traced. The order
of the stop, the exit and the count is handwritten in the walk, a copy of
what `rocketnet_server_msgx_stop`, `rocketnet_server_msgx_receive_worker` and
`rocketnet_server_msgx_stopped` do; the walk also computes the model's ghosts
`committedTop` and `late`, and a trace whose ghosts disagree with the model's
step is rejected. The unit test `test_rocketnet_msgx_receive_fill_stop`, in
tests/test_unit_msgx_receive_fill.cpp, covers the pure header's stop count.
The native paths (`rocketnet_server_msgx_stop`, `rocketnet_server_msgx_count_stopped`)
run only in the Mac functional cases of tests/functional/posix_socket_errors.cpp
(`retired_socket_counted`, `dual_stack_client_sockets_open`,
`finish_refused_counted`, `shared_receive_buffer_granted`), which stop the
transport and wait for it to report stopped. None of them checks the stopped
counts by value. The plant
(`ROCKETNET_MSGX_RECEIVE_FILL_PLANT`, `rocketnet_msgx_receive_want` in
`sdk/rocketnet_msgx_receive_fill.cpp` asking for a whole batch) must be
rejected at the first read that takes past the room. The reach predicates
`ShortRoom`, `Held`, `BudgetCaps` (a read capped by the budget), `ReadsShared`
(main has read from the bank the worker still fills) and `CallCut` (a call
left datagrams for the next) must each be reached by a clean trace.
`MCTraceRNMsgxReceiveFillSelf.cfg` and
`MCTraceRNMsgxReceiveFillSelfBrokenReadPastRoom.cfg` are the checker's own
controls. The unit tests (`tests/test_unit_msgx_receive_fill.cpp`) run the
same functions over the same kind of fake, with the review's 64 sockets of
150 datagrams, and the shared delivery rig of
`sdk/rocketnet_receive_delivery_check.h`; the functional case
`test_receive_latency_server_msgx` (tests/functional/receive_latency.cpp)
measures the same rule over real sockets.
