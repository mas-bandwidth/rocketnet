# Client disconnect burst through the link simulator

`RNClientLinkStop.tla` models one client's disconnect burst through the
development simulator. Disconnect applies loss and stages surviving copies.
Running updates drain due packets. What Stop does depends on the path:

- **Legacy path.** Stop transfers ownership by publishing quit; it does not
  drain or call the backend. Worker completion drains due and held copies
  before publishing stopped. Step-driven tests explicitly run completion.
- **I/O-core path.** The simulator is never handed to the I/O worker, so
  `rocketnet_client_transport_stop` drains it itself: it sends every copy it
  still holds, ahead of its latency (`rocketnet_client_link_drain` at
  `DBL_MAX`), synchronously on the caller's thread, then publishes quit.

`StopLeavesNothingStaged` applies when **stopped**, allowing packets to remain
queued during **stopping** on the legacy path. `StopDoesNotDrain` checks the
request per path: on the legacy path it changes neither the queue nor the
wire; on the I/O-core path it leaves nothing staged. (The property keeps its
legacy name.)

A copy leaves the simulator through the backend's send, which returns nothing:
the send can fail, and the network can lose the datagram. `ServerReceive` is
either the server reading a copy (`received`, and the first one frees the
slot) or the copy never arriving (`dropped`). `BurstAccounted` preserves every
copy across staged, sent, policy-lost, dropped and read. `BurstArrives` checks,
under worker/server fairness, that a burst with a copy surviving the loss
policy frees the server's slot unless every surviving copy is dropped on the
way; it does not claim delivery over a lossy send.

The registered cases use three copies: the fixed path must pass; the
missing-drain witness must violate `StopLeavesNothingStaged`; the due-only
witness must violate `BurstArrives` when latency leaves a copy held forever.
`MCRNClientLinkStopReachAllDropped.cfg` is a reach case, not a property of the
code: it breaks `BurstReachesServer` (delivery without the dropped exception),
showing that a burst whose every sent copy is dropped is in the model.
The former caller-drain model's 56-state receipt does not qualify this revision.
Fresh results must match this model's source and configurations.

The model abstracts queue ownership and delivery, not native mutex memory
ordering or backend blocking. It does not refine `ClientSession` or
`RNClientIoShutdown`, and excludes the I/O-core path without a simulator.
The deterministic unit tests cover no sends during repeated stop/poll calls,
exactly one completion drain, and eventual server slot release for both zero
and positive simulated latency.
