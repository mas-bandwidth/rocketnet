# RNLoopbackBindLayout

The loopback hub's bound table and its ephemeral port search, at the layout
of `sdk/rocketnet_driver_loopback.cpp` and with the code's own arithmetic.
[LoopbackHub](LoopbackHub.tla) checks the hub's registration lifecycle and
[LoopbackHubLayout](LoopbackHubLayout.tla) its queue rings; this model checks
how a datagram's destination is found: one lookup in `hub->bound` instead of a
walk of the endpoint records.

## State

| Model | Code |
|---|---|
| `live[r]`, `fam[r]`, `addr[r]`, `port[r]` | `hub->endpoints[r]`: `live` and `bind` (its type, its address as the family's wildcard or a specific one, its port) |
| `bound[f][p]` | `hub->bound[f == IPv6][p]` through `loopback_bound_entry`: the record index plus one of the live endpoint bound there, 0 for none |
| `nextPort` | `hub->next_port`, `ROCKETNET_LOOPBACK_FIRST_PORT` at hub create |
| `registered` | `hub->registered` |
| `First`, `Last` | `ROCKETNET_LOOPBACK_FIRST_PORT` (49152) and `ROCKETNET_LOOPBACK_LAST_PORT` (65535, the largest `uint16_t` port) |

Record 0 is the server's and records 1 onward the clients'. Because `Last`
stands for 65535, the code's `uint16_t` `port + 1` at the last port is
`(Last + 1) mod (Last + 1) = 0`, which is what a wrap without the test would
produce.

## Actions and the code they mirror

Functions in `sdk/rocketnet_driver_loopback.cpp`.

- **`Create(server, f, a, bp)`**: `loopback_create`. A bind to a family and
  port a live endpoint already holds is refused (the walk of the records,
  which under `BoundExact` is that family's entry for the port being nonzero); the server takes record 0 and a
  client the first free record after it, a full hub refusing. A bind to port
  0 searches from `next_port`, skipping every port either family holds and
  stepping with `loopback_next_port` (the last port wraps to the first), takes
  the port found and moves `next_port` past it. The record is written and the
  entry set to the record index plus one.
- **`Destroy(r)`**: `loopback_destroy`. The entry is cleared only when it
  names this record; the record is cleared and `registered` lowered.

`loopback_find_destination` changes nothing and is the operator `TableFind`:
the datagram family's entry, accepted when its endpoint is live and bound to
the wildcard or to the exact address; for IPv4, otherwise, the IPv6 entry
when its endpoint is a live IPv6 wildcard (dual stack).

## Invariants

- **`OneEndpointPerFamilyPort`**: no two live endpoints share a family and
  port.
- **`BoundExact`**: an entry is nonzero exactly when a live endpoint holds its
  family and port, and it names that endpoint.
- **`LookupIsWalk`**: for every family, destination address and port, the
  table lookup returns the endpoint a walk of the live records returns
  (`WalkFind`), the abstract contract the table replaces.
- **`EphemeralInRange`**: every port the search hands out is in
  `First..Last`.
- **`EphemeralPortFree`**: every port the search hands out is held by no live
  endpoint of either family.
- **`SearchEnds`**: the search always finds a port. The model assumes the
  range has more ports than the hub has records; no static assert in the
  code holds that, and its search stops after 65536 tries either way.
- **`NoPortZero`**, **`NextPortInRange`**: no live endpoint holds port 0, and
  `next_port` stays in the range.
- **`DestroyFindsOwnEntry`**: destroy's test always finds its own entry.
- **`RegisteredCount`**: `registered` is the number of live records.

`DestroyFindsOwnEntry` shows that the test in `loopback_destroy` is always
true under the code's create: create refuses a second endpoint of a family on
a port, so the entry at a live endpoint's family and port is always its own.
Clearing the entry without the test is therefore equivalent today, and is not
a reversed witness; the test keeps a later change to create from silently
unbinding another endpoint.

## The instance

`MCRNLoopbackBindLayout.cfg`: three records (a server and two clients), ports
0..5 with the ephemeral range 2..5 (four ports, one more than the records, as
the model assumes), explicit binds to port 1 (outside the range) and
port 2 (inside it, so an explicit bind and the search contend), binds to the
wildcard or a specific address in each family, and destinations at that
address or another.

## Reversed witnesses

Each turns one rule off and must fail with the invariant it names. Every
counterexample below was checked against the C by hand.

1. **`MCRNLoopbackBindLayoutBrokenNoPlusOne.cfg`** (`PlusOne = FALSE`): the
   entry stores the record index, not the index plus one. Violates
   `LookupIsWalk` in one step: the server binds IPv4 port 2 at a specific
   address and its entry is written as 0; `loopback_find_destination`'s
   `if ( same )` reads 0 as no endpoint, so a datagram to the server's own
   address and port finds nothing while the walk finds record 0.
2. **`MCRNLoopbackBindLayoutBrokenNoBindCheck.cfg`** (`BindCheck = FALSE`):
   `loopback_create` without its refusal of a live endpoint holding the
   bind's family and port. Violates `OneEndpointPerFamilyPort`: client
   record 1 binds IPv4 port 2, client record 2 binds IPv4 port 2 as well and
   overwrites the entry with 3, leaving record 1 bound but unreachable.
3. **`MCRNLoopbackBindLayoutBrokenWrapToZero.cfg`** (`WrapToFirst = FALSE`):
   `loopback_next_port` as `(uint16_t) ( port + 1 )` without the last-port
   test. Violates `EphemeralInRange`: an explicit bind holds port 2, port-0
   binds take 3 and 4, the holder of 4 is destroyed, the next port-0 bind takes
   5 and sets `next_port` to `5 + 1`, which wraps to 0; after that endpoint is
   destroyed the next search starts at 0, finds both families' entries for
   port 0 empty and binds port 0, whose entry then refuses every later port-0
   bind of that family at create's first test.
4. **`MCRNLoopbackBindLayoutBrokenOneFamily.cfg`** (`BothFamilies = FALSE`):
   the search loop counting a port held only when an endpoint of the bind's
   own family holds it (`bind.type` compared too in its held test). Violates `EphemeralPortFree`: client record 1 binds
   IPv4 port 2 explicitly, client record 2 binds IPv6 port 0 and the search
   from 2 tests only the IPv6 entry, so the ephemeral IPv6 endpoint shares
   port 2 with the IPv4 one.

## Runs

The records are in `RUNS.tsv` (group `rnloopbackbindlayout`), made with the
pinned runner as `proofs/tla/README.md` describes.
