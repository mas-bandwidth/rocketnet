# Client disconnect burst through the link simulator

`RNClientLinkStop.tla` models one client's disconnect burst when the
development link simulator is on (`sdk/rocketnet_client_transport.cpp`).

With a link simulator the receive worker does not send the burst.
`rocketnet_client_transport_disconnect` applies the loss policy to each copy
and stages the rest in the simulator's queue. Only the caller's thread drains
that queue, in a running update (`rocketnet_client_link_drain`). The public
`rocketnet_client_disconnect` requests stop right after the disconnect. Once
stop is requested an update only finishes the stop and never drains again.
The shutdown wait helpers pass a fixed time (1000.0), so a copy still inside
its latency never comes due after stop.

Stop therefore sends every staged copy, due or held
(`rocketnet_client_link_drain_at_stop`, called from
`rocketnet_client_transport_stop`).

## Variables and actions

| Variable | Meaning |
| --- | --- |
| `phase` | `running`, `stopping` (stop requested), `stopped` |
| `sealed` | the burst was sealed |
| `due`, `held` | staged copies past or inside their latency |
| `wire`, `received` | copies in the server's receive queue, copies it read |
| `lost` | copies the loss policy removed at send |
| `freed` | the server freed the slot on a copy |

`Disconnect`, `Tick` (a running update advances the clock), `Drain`, `Stop`,
`WorkerComplete`, `ServerReceive`. Stop is not fair (the caller chooses it).

Invariants `TypeOK`, `BurstAccounted`, `StopLeavesNothingStaged`; liveness
`BurstArrives`: a burst with any surviving copy reaches the server.

## Cases (Copies = 3)

- `MCRNClientLinkStop.cfg`: passes, 56 distinct states.
- `MCRNClientLinkStopBrokenNoDrain.cfg` (`Defect = "nodrain"`, the code before
  the fix): `StopLeavesNothingStaged` fails. TLC's trace: Disconnect stages one
  held copy and loses two; Stop leaves it staged. By hand against the code:
  `rocketnet_client_transport_disconnect` puts the burst through
  `rocketnet_client_link_send` into the simulator; `rocketnet_client_transport_stop`
  stored quit; `rocketnet_client_transport_update` returned at its
  `stop_requested` branch before `rocketnet_client_link_drain`. The unit test
  `test_client_disconnect_sends_burst_staged_in_link_simulator` failed there
  (no disconnect packet received), and `example_soak` freed no slot until the
  10 second timeout.
- `MCRNClientLinkStopBrokenDueOnly.cfg` (`Defect = "dueonly"`, stop drains at
  the caller's clock): `BurstArrives` fails. TLC's trace: Disconnect stages two
  held copies and loses one; Stop sends none; WorkerComplete; stutter. By hand:
  a drain through `rocketnet_network_simulator_receive_any` at the transport's
  time leaves a copy with latency, and the wait helpers' fixed 1000.0 never
  advances it; the unit test's 50 ms case covers this.

The model is standalone: it does not refine `ClientSession` or
`RNClientIoShutdown`, and it does not cover the I/O-core path, which never has
a link simulator.
