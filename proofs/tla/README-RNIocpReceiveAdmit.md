# RNIocpReceiveAdmit

The IOCP server's completion worker admitting one received datagram to its
receive bank (`rocketnet_iocp_receive_admit` and `rocketnet_iocp_bank_datagram`
in `sdk/rocketnet_server_iocp_windows.cpp`). The worker runs the shared
stateless prefilter (`sdk/rocketnet_receive_prefilter.h`) first, then the
capacity check. So a datagram that main's stateless rungs would refuse never
takes a slot, and a full bank refuses only datagrams the prefilter accepted.

The blast row this answers (FINDING-RECEIVE-EVICTION, base-B1b in
TRANSPORT-PUSH-iocp.md) measured the old order: the capacity check came first
and the prefilter ran on main after banking. A garbage flood filled the bank,
the players' datagrams were dropped as bank-full, and those drops were never
exported.

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
| `Drain` | `rocketnet_server_iocp_receive_packets`. The RNBankFifoLayout hand-off, which [RNBankFifoLayout](README-RNBankFifoLayout.md) checks at its layout, is one step here. |

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
| `MCRNIocpReceiveAdmitBrokenBankFirst.cfg` | the capacity check first, as the blast row measured: `NoEvictionByGarbage` fails |
| `MCRNIocpReceiveAdmitBrokenUncountedFull.cfg` | a bank-full drop not counted: `EveryDropCounted` fails |
| `MCRNIocpReceiveAdmitBrokenCheckPastEnd.cfg` | `num_packets <= capacity`: `InBounds` fails |

The unit tests `tests/test_unit_iocp_receive_admit.cpp` run the code's
admission over the same scenario: a bank of four meets sixteen garbage
datagrams and then four players' datagrams.
