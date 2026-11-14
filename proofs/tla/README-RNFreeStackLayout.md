# RNFreeStackLayout: the protocol tables' free stacks at the code's layout

`RNFreeStackLayout.tla` models the dense free stacks of three transport
protocol tables: an array of slot indices and a count, filled at create so the
first pops return 0, 1, 2, popped at `free[count - 1]` and pushed at
`free[count]`. The open-addressed indexes beside them are `RNOpenAddress.tla`'s.
The three take shapes run on one stack here, a superset of each table's.

The client-slots stack fits this layout only for its take's guard order. Its
release holds the index above the top, at `free_stack[free_stack_count +
free_stack_held++]`, until `rocketnet_client_slots_publish` makes it
poppable, and its pop moves the last held entry down into the popped place.
That stack is modelled by `RNClientSlotsLayout.tla` (`Assign`,
`ReleaseHandle`, `Release`, `Publish`); `StackIsFree` as stated here does not
hold for it between a release and the publish.

## The instance

`MCRNFreeStackLayout.tla`: capacity 3, map generations to 4, owner
generations 1 and 2.

## Actions and the code they mirror

| Action | Code |
|---|---|
| `TakeMap` | `rocketnet_encryption_map_add` (`sdk/rocketnet_encryption_map.cpp`): refused when full (`num_free == 0` or the next generation 0), pops `free_indices[--num_free]`, tags the generation, which steps from its maximum to the exhausted 0 |
| `TakeSlots(g)` | the guard order of `rocketnet_client_slots_assign` (`sdk/rocketnet_client_slots.cpp`): peeks the top, refuses a slot whose generation is already `g`, then pops (the held entries are `RNClientSlotsLayout.tla`'s) |
| `PeekClaim` | `rocketnet_used_token_history_claim_record` (`sdk/rocketnet_token_tables.cpp`): `rn_token_claim_select_slot` selects the top while `free_count > 0`, else the oldest claim whose deadline has passed (a live slot; any live slot here, since the model keeps no deadlines or order) |
| `PopClaim` | the rest of the same call: `--free_count` only when `free_count > 0` and the slot is the top; else `rn_token_slot_retire` and the insert, so the slot stays live and the stack is untouched |
| `Release(i)` | `rocketnet_encryption_map_remove_generation`, `rn_token_free_push`: only a live slot is pushed (not `rocketnet_client_slots_release`, which holds the index until publish: `RNClientSlotsLayout.tla`) |

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
| `MCRNFreeStackLayoutBrokenPopExpired.cfg` | the claim pops the stack for the expired live slot it reuses, not only for a slot taken from it | `StackIsFree` |

Each counterexample was checked against the C by hand: no-guard takes slot 0
and releases slot 1, which is pushed twice; dec-first takes and releases slot
0 under generation 1, then a take with owner 1 is refused after the pop, and
slot 0 is neither live nor free; wrap-one hands out 1 to 4 and then 1 again.
Pop-expired takes slots 0, 1 and 2, so the stack is empty; a claim then
selects live slot 0 (the code's oldest expired claim) and pops anyway:
`count = -1`, where `rocketnet_used_token_history_claim_record` takes its else
arm (`free_count > 0` is false) and retires the slot without touching
`free_count`. PeekClaim and PopClaim are the two halves of that one call, so
no other operation runs between them in either case.
