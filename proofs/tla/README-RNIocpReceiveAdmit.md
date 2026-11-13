# RNIocpReceiveAdmit

The IOCP server's completion worker admitting one received datagram to its
receive bank (`rocketnet_iocp_receive_admit` and `rocketnet_iocp_bank_datagram`
in `sdk/rocketnet_server_iocp_windows.cpp`). The worker runs the shared
stateless prefilter (`sdk/rocketnet_receive_prefilter.h`) first, then the
capacity check. So a datagram that main's stateless rungs would refuse never
takes a slot, and a full bank refuses only datagrams the prefilter accepted.

The order it replaces put the capacity check first and ran the prefilter on
main after banking: a garbage flood filled the bank, the players' datagrams
were dropped as bank-full, and those drops were never exported. The code that
keeps the order is `rocketnet_iocp_receive_admit` (prefilter, then
`num_packets < capacity`) and `rocketnet_iocp_bank_datagram` (every refusal
counted in the worker's drop tally); `tests/test_unit_iocp_receive_admit.cpp`
runs that admission over the flood (below), and `MCRNIocpReceiveAdmitBrokenBankFirst.cfg`
is the old order as a reversed witness.

## State

| Model | Code |
|---|---|
| `bank` | the fill bank's `data[0 .. num_packets - 1]`, in fill order; `Capacity` is `rocketnet_server_iocp_receive_queue_size( max_clients, workers )` |
| `refused[k]` | the worker's drop tally, by the prefilter's reason (`rocketnet_receive_drop_tally_count`) |
| `full[k]` | `ROCKETNET_RECEIVE_DROP_BANK_FULL` in the same tally, folded into `ROCKETNET_COUNTER_RECEIVE_DROPPED_FULL` through the `receive_drop_totals` operation |
| `counted[k]` | what the tally holds; `RECEIVE_DROPPED` counts the same datagrams |
| `delivered[k]` | what main took from the bank |
| `evicted` | ghost: a player's datagram refused bank-full while garbage held a slot |

Kinds: a player's datagram; garbage, which fails a stateless rung (a wrong
filter value, a size its type forbids, and the rest of
`rocketnet_receive_drop_t`); and hostile, which is well formed and passes the
prefilter, and which only main's keyed rungs refuse.

## Actions

| Action | Code |
|---|---|
| `Refuse(k)`, `Bank(k)` or `Full(k)`, by the verdict | `rocketnet_iocp_receive_admit`, then `rocketnet_iocp_bank_datagram` banks the datagram or counts it |
| `Drain` | the worker's bank leaving it: over the receive handoff (`sdk/rocketnet_receive_handoff.h`, which `RNReceiveHandoff.tla` checks) `rocketnet_server_iocp_receive_packets` starts on the bank and the worker moves to an empty one at its next commit; one step here. |

## Invariants

- `InBounds`: `num_packets` never passes the capacity.
- `Accounted`: every datagram is in the bank, delivered, or dropped.
- `EveryDropCounted`: every drop is counted under a named reason.
- `NoGarbageBanked`: once the filter is published, garbage never takes a slot.
- `NoEvictionByGarbage`: garbage never causes a player's datagram to be refused.

A player's datagram can still be refused bank-full when the bank is full of
well-formed datagrams, whether players' or hostile. That case is the
known-source share's, not this layer's.

## Instances

| Config | Expected |
|---|---|
| `MCRNIocpReceiveAdmit.cfg` | three slots, seven datagrams, filter published: pass |
| `MCRNIocpReceiveAdmitCap4.cfg` | four slots, eight datagrams: pass |
| `MCRNIocpReceiveAdmitFilterOff.cfg` | before the filter is published: the bounds and the counts hold, pass |
| `MCRNIocpReceiveAdmitBrokenBankFirst.cfg` | the capacity check first, the old order: `NoEvictionByGarbage` fails |
| `MCRNIocpReceiveAdmitBrokenUncountedFull.cfg` | a bank-full drop not counted: `EveryDropCounted` fails |
| `MCRNIocpReceiveAdmitBrokenCheckPastEnd.cfg` | `num_packets <= capacity`: `InBounds` fails |

The unit tests `tests/test_unit_iocp_receive_admit.cpp` run the code's
admission over the same scenario: a bank of four meets sixteen garbage
datagrams and then four players' datagrams.

## What the trace binds

The trace layer `iocp-admit` (`tests/tla_trace_iocp.cpp`) calls only the
verdict: `rocketnet_iocp_receive_admit`, through its development seam
`rocketnet_server_iocp_test_receive_admit`, with the bank's fill and capacity
handed in. The bank, the tallies (`arrived`, `refused`, `full`, `counted`,
`delivered`) and `evicted` are the walk's own, kept from the verdict as
`rocketnet_iocp_bank_datagram` keeps them: neither `rocketnet_iocp_bank_datagram`
(`num_packets++`, the drop tally) nor `rocketnet_server_iocp_receive_packets`
is called. So the trace checks that the code's verdict is the model's for
each kind and fill; `InBounds`, `Accounted` and `EveryDropCounted` over the
code's own bank and tally rest on reading `rocketnet_iocp_bank_datagram`
(every non-`ACCEPT` verdict is counted before it returns). Driving the real
bank and receive path is a roadmap entry.
