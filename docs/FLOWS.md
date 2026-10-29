# Flows

Flows are a planned server-side grouping layer for applications that need to
send the same message to a changing set of connected clients. They are not part
of the public API in this release. Today an application broadcasts by
iterating connected client slots and calling `rocketnet_server_start_message`
and `rocketnet_server_finish_message` for each destination.

A future flow implementation must preserve the existing message contracts:

- a subscription belongs to a connection generation, never merely to a
  reusable slot;
- sends reach only connections subscribed when the send begins;
- destroyed flow ids cannot resolve to a later flow;
- tables have fixed create-time capacity and frame calls allocate nothing;
- every refusal is counted and leaves state unchanged;
- flow operations run on the server object's owning thread;
- sending to a flow is equivalent to one ordinary per-client send for each
  current subscriber, including budget and receipt behavior.

`proofs/tla/RNFlows.tla` records these state-machine requirements. Public names,
configuration and counters require a future API revision before this planned
layer can ship.
