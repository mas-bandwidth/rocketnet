# Flows: server-side subscription groups

A flow is a set of connected slots on the message server, named by an id.
A send to a flow queues one copy of the message to each subscriber, the way
`rocketnet_message_server_broadcast` queues one copy to each connected slot.
The game creates a flow per chunk (a square of its top-down map), subscribes
each client to the chunks near its player, and sends a chunk's updates to the
chunk's flow (REQUIREMENTS.md, Flows).

This is a layer-8 addition to docs/CHANNELS.md: it sits on the message server
object (CHANNELS.md 11.1 and 11.5) and uses only its per-slot send. Section
11.4 of CHANNELS.md says a flow send is a loop over subscribers and that
nothing in the objects forecloses it; this document settles the rest. The
state machine is `tla/RNFlows.tla` (`tla/README-RNFlows.md`). No code
implements it yet; section 6 lists the cards that do.

## 1. Rules

- **F1. A subscription belongs to a connection, not to a slot.** It records
  the connection id of its slot at subscribe time (the id RNSlots binds the
  slot's endpoint to, `tla/RNSlots.tla` Observe). When that connection ends,
  every subscription it holds ends before the application can send again.
  A later connection in the same slot starts with no subscriptions
  (`NoSubscriptionSurvivesReuse`).
- **F2. A send reaches only connections subscribed when it was sent**
  (`DeliveredOnlyToSubscribers`). An unsubscribe takes effect for every send
  after it; a destroy takes effect for every send after it.
- **F3. Flow ids never repeat.** A destroyed flow's id never resolves to the
  flow that later takes its table index, so a stale id reaches nobody.
- **F4. Fixed memory.** Every table is sized from the configuration at
  create, inside the server's one block. Nothing is allocated after create
  (CHANNELS.md R2).
- **F5. Every refusal is counted** under its own name and changes no flow
  state.
- **F6. Main thread only.** Create, destroy, subscribe, unsubscribe and send
  run on the main thread between updates, the same rule as
  `rocketnet_message_server_send`.

## 2. Bounds and memory

Three server-side fields join the message configuration. They are not wire
fields: a client ignores them, and the CONFIG announcement (CHANNELS.md 10.3)
does not carry them.

| Field | Default | Range | Meaning |
|---|---:|---|---|
| `max_flows` | 0 | 0 to 65536 | live flows at once; 0 means flows are off and take no memory |
| `max_flow_subscribers` | 1024 | 1 to 65536 | subscribers of one flow; the effective bound is `min(max_flow_subscribers, max_clients)` |
| `max_client_flows` | 64 | 1 to 4096 | flows one connection may be subscribed to at once |

Symbols: `N = max_clients`, `Fl = max_flows`, `K = max_client_flows`.

| Part | Entry | Bytes |
|---|---|---|
| flow table | `{ uint64 id; int32 head; int32 count; }` | `16·Fl` |
| subscription pool, one row of `K` per slot | `{ uint64 connection_id; int32 flow; int32 prev; int32 next; int32 pad; }` | `24·K·N` |
| per-slot row count | `int32` | `4·N` |

```
flow_bytes = 16·Fl + 24·K·N + 4·N      (each part 64-aligned; 0 when Fl = 0)
```

A subscription is the node at row `slot`, column `j` of the pool, so the
total subscriptions can never exceed `K·N` and need no free list. A flow's
subscribers are a doubly linked list through the pool, headed in the flow
table. At `N = 256`, `Fl = 1024`, `K = 64`: `16 KiB + 384 KiB + 1 KiB`, about
401 KiB. `rocketnet_message_server_required_memory` adds `flow_bytes`, and
`rocketnet_message_connection_sizing` prints the line.

**Ids.** A flow id is `(serial << 32) | (index + 1)`. `serial` is the
server's create counter, from 1, so an id is never 0 and never repeats.
Resolving an id is one range check and one compare: `index = (id & 0xffffffff)
- 1`, then `table[index].id == id`. After 2^32 - 1 creates, create refuses
with `FLOW_IDS_EXHAUSTED`; the model's `MaxIds` is this bound.

## 3. The API

In `sdk/rocketnet.h`, message objects section, server part:

```c
// a new flow's id, nonzero; 0 refused (FLOW_TABLE_FULL, FLOW_IDS_EXHAUSTED, flows off).
uint64_t rocketnet_message_server_flow_create( struct rocketnet_message_server_t * server );

// 1 when the flow existed; it and all its subscriptions are gone. 0 refused (FLOW_UNKNOWN).
int rocketnet_message_server_flow_destroy( struct rocketnet_message_server_t * server, uint64_t flow );

// 1 when client_index's current connection is now subscribed; 0 refused (section 4).
int rocketnet_message_server_flow_subscribe( struct rocketnet_message_server_t * server, uint64_t flow, int client_index );

// 1 when it was subscribed and is not now; 0 refused (FLOW_UNKNOWN, FLOW_NOT_SUBSCRIBED).
int rocketnet_message_server_flow_unsubscribe( struct rocketnet_message_server_t * server, uint64_t flow, int client_index );

// Queues data to every subscriber exactly as rocketnet_message_server_send
// would, one copy per connection. Returns the number of subscribers it was
// queued for; a subscriber whose send path refuses is skipped and counted by
// that path, as broadcast does. 0 for a stale id (FLOW_UNKNOWN).
int rocketnet_message_server_flow_send( struct rocketnet_message_server_t * server, uint64_t flow,
                                        int channel, int reliability, const uint8_t * data, int bytes );
```

A NULL server or data is a counted no-op returning 0, as for every other
server call. Each export also goes in `tools/exports.txt`.

Underneath is a pure module, `rn_flows_t` in `sdk/rocketnet_flows.h`, that
owns the tables and never calls the transport or the endpoints:

```c
uint64_t rn_flows_bytes( int max_clients, int max_flows, int max_client_flows );
void     rn_flows_init( rn_flows_t *, void * block, int max_clients, int max_flows,
                        int max_flow_subscribers, int max_client_flows );
uint64_t rn_flows_create( rn_flows_t *, int * reason );
int      rn_flows_destroy( rn_flows_t *, uint64_t flow, int * reason );
int      rn_flows_subscribe( rn_flows_t *, uint64_t flow, int slot, uint64_t connection_id, int * reason );
int      rn_flows_unsubscribe( rn_flows_t *, uint64_t flow, int slot, int * reason );
void     rn_flows_drop_slot( rn_flows_t *, int slot );                      // F1
int      rn_flows_first( const rn_flows_t *, uint64_t flow, int * slot, uint64_t * connection_id ); // walk for send
int      rn_flows_next( const rn_flows_t *, int * cursor, int * slot, uint64_t * connection_id );
```

`reason` is the counter index to bump; the server bumps it, the module stays
pure (CHANNELS.md 16.3).

## 4. Refusals

Each is a counter appended to `sdk/rocketnet_counters.h` (indices are the
contract), class REFUSAL unless noted. The checks run in this order, the
order `tla/RNFlows.tla` Subscribe takes them.

| Counter | When |
|---|---|
| `FLOW_TABLE_FULL` | create with `max_flows` live flows (or flows off) |
| `FLOW_IDS_EXHAUSTED` | create after 2^32 - 1 creates |
| `FLOW_UNKNOWN` | destroy, subscribe, unsubscribe or send with an id that is 0, out of range, or not live (destroyed) |
| `FLOW_NOT_CONNECTED` | subscribe of a client index out of range, or of a slot with no bound connection |
| `FLOW_ALREADY_SUBSCRIBED` | subscribe of a connection already in the flow |
| `FLOW_SUBSCRIBERS_FULL` | subscribe to a flow at its subscriber bound |
| `FLOW_CLIENT_FLOWS_FULL` | subscribe of a connection at `max_client_flows` |
| `FLOW_NOT_SUBSCRIBED` | unsubscribe of a connection not in the flow |
| `FLOW_STALE_SUBSCRIPTION` (class EXPECTED, always 0) | send meets a subscription whose connection id is not its slot's current one; it is skipped. F1 makes this unreachable (the model sends with no such check and stays safe); the counter and a debug assert keep it visible |

A refusal changes no flow state (F5); in the model each is a stutter step.
A send's per-subscriber refusals (exhausted, over budget, not connected) are
the send path's own counters, unchanged.

## 5. Disconnect and slot reuse

`rocketnet_message_server_update` already observes every slot after the
transport's update (RNSlots Observe, `sdk/rocketnet_message_server.cpp`).
For each slot whose observed connection changed (RESET or DROP), it calls
`rn_flows_drop_slot( slot )` before resetting the endpoint. That walks the
slot's row of at most `K` nodes, unlinks each from its flow and zeroes the
row. `tla/RNFlows.tla` Observe is this step.

- **Disconnect.** The slot's connection goes to 0; its subscriptions end at
  the next update's Observe. Between the disconnect and that Observe the
  application cannot send (sends happen between updates, and the transport
  only changes slots inside an update), so no send sees the stale
  subscription.
- **Kick.** `rn_message_object_kick` (exhaustion, wire violation, reliable
  timeout) also calls `rn_flows_drop_slot`, so a kicked connection leaves
  its flows at once rather than at the next Observe.
- **Reuse.** A new connection in the slot is a changed connection id, so
  Observe drops first and resets second; the new connection starts with an
  empty row. Subscriptions are never carried to it (F1); the game
  resubscribes it when it places the player.
- **Destroy** walks the flow's list and frees each node in its slot's row.
- **The known gap.** The object's connection id today is the transport's
  `client_id` for the slot (CHANNELS.md 11.5, Admission). A reconnect of the
  same client id into the same slot inside one transport update is not a
  change, so neither the endpoint nor the flow row resets. The transport's
  per-connection id (CHANNELS.md P9a) closes it for both at once; flows add
  no gap of their own.

## 6. The cards

Ordered; each depends on the one before. Each test is a unit test in a root
`test_unit_*.cpp` (no sockets, no real time, the clock as an argument from
1000.0) unless named functional.

1. **rocketnet-flows-table.** `sdk/rocketnet_flows.h`,
   `sdk/rocketnet_flows.cpp`, `test_unit_flows.cpp`, `tla/COVERAGE.tsv`
   (rn-flows owed to current, code `sdk/rocketnet_flows.cpp`). The pure
   `rn_flows_t` of section 3, each function citing its `tla/RNFlows.tla`
   action. Tests: `flows_create_until_table_full`,
   `flows_destroyed_id_is_unknown_after_index_reuse` (F3),
   `flows_subscribe_refusals_in_order` (section 4),
   `flows_unsubscribe_stops_later_sends` (the
   `MCRNFlowsBrokenSendAfterUnsubscribe` trace, red with the list left in
   place), `flows_drop_slot_ends_every_subscription` (F1, the reuse trace),
   `flows_bytes_matches_formula`.
2. **rocketnet-flows-config-and-counters.** `sdk/rocketnet.h` (the three
   fields of section 2, not WIRE), `sdk/rocketnet_counters.h` (the nine
   counters, appended), `sdk/rocketnet_message_server.cpp` (required memory
   and sizing add `flow_bytes`), `test_unit_flows_config.cpp`. Tests:
   `flows_config_ranges_refused` (each field one past each end),
   `flows_off_adds_no_memory`, `flows_required_memory_matches_formula`.
3. **rocketnet-flows-server-api.** `sdk/rocketnet.h` (the five calls),
   `sdk/rocketnet_message_server.cpp` (carve the block at create, the
   Observe and kick drops of section 5, the five calls over `rn_flows_t` and
   the per-slot send), `tools/exports.txt`,
   `test_unit_message_server_flows.cpp` over a fake object link (the
   `link` callbacks of `rn_message_object_t`), time passed in. Tests:
   `server_flow_send_reaches_only_subscribers`,
   `server_flow_subscription_ends_at_disconnect`,
   `server_flow_reused_slot_starts_unsubscribed`,
   `server_flow_kick_leaves_flows`, `server_flow_null_guards_counted`.
4. **rocketnet-flows-functional.** `functional/rocketnet_functional_tests.cpp`
   and `docs/FLOWS.md` (an as-built note). Functional test
   `functional_flows_chunk_subscribers_over_loopback`: three clients, two
   chunk flows, a client moving between chunks by unsubscribe and subscribe,
   a client disconnecting and another taking its slot; every message reaches
   exactly the clients subscribed when it was sent.

## 7. Not decided here

- A flow send through the zero-copy start/finish path (one region copy per
  subscriber is the R3 copy broadcast also makes).
- Sends from fill-phase threads.
- A subscriber count or iteration call for the game; the game keeps its own
  chunk membership.
