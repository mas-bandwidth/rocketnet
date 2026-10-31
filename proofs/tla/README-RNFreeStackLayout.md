# RNFreeStackLayout: the protocol tables' free stacks at the code's layout

`RNFreeStackLayout.tla` models the dense free stacks of three transport
protocol tables: an array of slot indices and a count, filled at create so the
first pops return 0, 1, 2, popped at `free[count - 1]` and pushed at
`free[count]`. The open-addressed indexes beside them are `RNOpenAddress.tla`'s.
The three take shapes run on one stack here, a superset of each table's.

## The instance

`MCRNFreeStackLayout.tla`: capacity 3, map generations to 4, owner
generations 1 and 2.

## Actions and the code they mirror

| Action | Code |
|---|---|
| `TakeMap` | `rocketnet_encryption_map_add` (`sdk/rocketnet_encryption_map.cpp`): refused when full (`num_free == 0` or the next generation 0), pops `free_indices[--num_free]`, tags the generation, which steps from its maximum to the exhausted 0 |
| `TakeSlots(g)` | `rocketnet_client_slots_assign` (`sdk/rocketnet_client_slots.cpp`): peeks the top, refuses a slot whose generation is already `g`, then pops |
| `TakeClaim` | `rocketnet_used_token_history_claim_record` (`sdk/rocketnet_token_tables.cpp`): the top selected, popped while it is still the top |
| `Release(i)` | `rocketnet_encryption_map_remove_generation`, `rocketnet_client_slots_release`, `rn_token_free_push`: only a live slot is pushed |

## Invariants

| Invariant | Meaning |
|---|---|
| `StackIsFree` | the stack below `count` holds every slot that is not live, each once |
| `GenerationNeverReissued` | a map generation is never handed out twice, and never 0 |

## Reversed witnesses

| Config | Rule turned off | Fails |
|---|---|---|
| `MCRNFreeStackLayoutBrokenNoGuard.cfg` | a release of a slot that is not live is pushed | `StackIsFree` |
| `MCRNFreeStackLayoutBrokenDecFirst.cfg` | the client-slots take pops before the generation test | `StackIsFree` |
| `MCRNFreeStackLayoutBrokenWrapOne.cfg` | the generation steps from its maximum to 1 | `GenerationNeverReissued` |

Each counterexample was checked against the C by hand: no-guard takes slot 0
and releases slot 1, which is pushed twice; dec-first takes and releases slot
0 under generation 1, then a take with owner 1 is refused after the pop, and
slot 0 is neither live nor free; wrap-one hands out 1 to 4 and then 1 again.
