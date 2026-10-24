# Client disconnect burst through the link simulator

`RNClientLinkStop.tla` models one client's disconnect burst through the
development simulator. Disconnect applies loss and stages surviving copies.
Running updates drain due packets. Stop transfers ownership by publishing quit;
it does not drain or call the backend. Worker completion drains due and held
copies before publishing stopped. Step-driven tests explicitly run completion.

`StopLeavesNothingStaged` applies when **stopped**, allowing packets to remain
queued during **stopping**. `StopDoesNotDrain` checks that the request changes
neither the queue nor the wire. `BurstAccounted` preserves every copy;
`BurstArrives` checks delivery of a surviving copy under worker/server fairness.

The three registered cases use three copies: the fixed path must pass; the
missing-drain witness must violate `StopLeavesNothingStaged`; the due-only
witness must violate `BurstArrives` when latency leaves a copy held forever.
The former caller-drain model's 56-state receipt does not qualify this revision.
Fresh results must match this model's source and configurations.

The model abstracts queue ownership and delivery, not native mutex memory
ordering or backend blocking. It does not refine `ClientSession` or
`RNClientIoShutdown`, and excludes the I/O-core path without a simulator.
The deterministic unit tests cover no sends during repeated stop/poll calls,
exactly one completion drain, and eventual server slot release for both zero
and positive simulated latency.
