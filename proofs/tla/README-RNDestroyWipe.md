# RNDestroyWipe

What server destroy writes, in `sdk/rocketnet_api.cpp`
(`rocketnet_server_destroy`), `sdk/rocketnet_server_transport.cpp`
(`rocketnet_server_transport_destroy`) and `sdk/rocketnet_encryption_map.cpp`
(`rocketnet_encryption_map_remove`, `rocketnet_encryption_map_destroy`) and
`sdk/rocketnet_client_slots.cpp` (`rocketnet_client_slots_release`,
`rocketnet_client_slots_destroy`). The model is the server side. The client's
wipes (`rocketnet_client_protocol_wipe_secrets` at the connection reset and at
the client transport's destroy) and the client I/O's copy of the open key are
not modelled.

Destroy used to wipe the whole pre-allocated block once a start had used it,
touching every page of a plan that reaches gigabytes: at 1000 clients the
example soak's shutdown peak was 29.4 GB against an 18.4 GB run, and the
Linux soak was OOM-killed. Destroy now wipes exactly the secrets and frees the
block as it is; the OS zero-fills a page before another process gets it.

## Pages

Each secret has a page: the server keys on `KeyPage`, the disconnect ring's
key array on `RingPages`, a mapping's entry on `EntryPage[m]` and its
client's slot on `SlotPage[m]`. Every write in the model touches the page it
writes, destroy's wipes included. Nothing in a run gives a page of the
pre-allocated block back, so the pages written since create (`touched`) are
the pages resident, and the run's peak is the pages written before destroy
(`runTouched`). `ShutdownPeakWithinRun` can therefore fail when destroy
wipes an object the run never wrote: its page shows in `touched` and not in
`runTouched`, which is what `MCRNDestroyWipeBrokenWipeEverySlot.cfg` shows.
The model has no byte offsets or lengths: it checks which objects destroy
wipes, not the extent of each wipe, so a wipe that overran its object into
an unused page is not visible here. The extents are held by
`test_unit_secrets_wiped.cpp`, which checks that destroy writes at most a
handful of bytes of any allocation. The model counts
writes only: destroy also reads `connected[]` and the map's addresses over
their whole capacity, and those reads are not modelled.

The ring key copy is kept per mapping in the model, so `Add(m)` waits until
`m`'s earlier burst has left the ring. The code keys the copy by ring entry
and can reuse a freed encryption-map index while the old copy still waits;
that reuse is not explored.

## Actions

- **Init**: create writes the server keys and zeroes the ring's key array
  (`memset` in `rocketnet_server_transport_allocate`), so `KeyPage` and
  `RingPages` are resident from the start.
- **Use(p)**: a connection writes a page of the block.
- **Add(m)**: a mapping is written whole, its session keys, nonce and
  connect-token user data with it, and the client slot takes a copy of the
  user data. The user data comes out of the token's private part and can
  carry the game's secrets. `rocketnet_encryption_map_add` and
  `rocketnet_client_slots_assign` are folded into one step; nothing reads
  between them.
- **Remove(m)**: the mapping's session keys, nonce and user data are wiped and
  it leaves live; the slot's release wipes its copy. Its second arm is the
  reset after `disconnect_client` deferred the burst
  (`rocketnet_server_transport_defer_disconnect_burst`), which first copies
  the mapping's server-to-client session key into the ring's key array: that
  copy outlives the mapping while the burst waits.
- **BurstLeaves(m)**: the drain or the stop step pops a complete or abandoned
  burst from the ring head and wipes its key copy (`sodium_memzero` on
  `disconnect_burst_keys[disconnect_burst_head]`).
- **Destroy**: the server secret key and both cookie keys are wiped, the
  secrets of every live mapping (`rocketnet_encryption_map_destroy` visits
  entries whose address is set), the user data of the connected slots only
  (`rocketnet_client_slots_destroy`; a released slot was wiped at release and
  an unassigned one was never written), and the whole ring key array
  (`rocketnet_server_transport_destroy`); nothing else is written.

## Invariants

- **`SecretsZeroAtFree`**: when the block is freed, no secret is set, ring
  key copies included.
- **`ShutdownPeakWithinRun`**: destroy touches no page the run did not, so the
  shutdown's resident peak is never above the run's.
- **`NoSecretOutsideLive`**: while running, only a live mapping and its slot
  hold secrets.
- **`RingCopyOnlyWhileWaiting`**: while running, a ring key copy is set only
  while its burst waits. This is the bounded retention the ring allows past
  the mapping's removal.
- **`TypeOK`**.

## Cases

| config | constants | expected |
|---|---|---|
| `MCRNDestroyWipe.cfg` | six pages, two mappings, every rule on | pass |
| `MCRNDestroyWipeBrokenWholeBlock.cfg` | `WipeWholeBlock = TRUE` | `ShutdownPeakWithinRun` violated |
| `MCRNDestroyWipeBrokenNoRemoveWipe.cfg` | `WipeOnRemove = FALSE` | `NoSecretOutsideLive` violated |
| `MCRNDestroyWipeBrokenSkipLive.cfg` | `WipeLiveAtDestroy = FALSE` | `SecretsZeroAtFree` violated |
| `MCRNDestroyWipeBrokenNoReleaseWipe.cfg` | `WipeSlotOnRelease = FALSE` | `NoSecretOutsideLive` violated |
| `MCRNDestroyWipeBrokenNoSlotsDestroyWipe.cfg` | `WipeSlotsAtDestroy = FALSE` | `SecretsZeroAtFree` violated |
| `MCRNDestroyWipeBrokenWipeEverySlot.cfg` | `WipeEverySlot = TRUE` | `ShutdownPeakWithinRun` violated |
| `MCRNDestroyWipeBrokenRingKeptOnLeave.cfg` | `WipeRingOnLeave = FALSE` | `RingCopyOnlyWhileWaiting` violated |
| `MCRNDestroyWipeBrokenRingNotWipedAtDestroy.cfg` | `WipeRingAtDestroy = FALSE` | `SecretsZeroAtFree` violated |

Each witness was checked by hand against the code:

- **Whole block** (Init, then Destroy): the old `rocketnet_server_destroy`
  zeroed `rn_server_header_bytes + block_bytes` whenever a start had set
  `block_used`, whatever the run touched.
- **No remove wipe** (Add, Remove): a remove that only clears `live` leaves the
  session keys in an entry that destroy no longer visits, since destroy wipes
  the live entries only.
- **Skip live** (Add, Destroy): stop no longer resets each slot before the
  message server goes, so a mapping can still be live at destroy; without the
  live loop in `rocketnet_encryption_map_destroy` its keys are freed unwiped.
- **No release wipe** (Add, Remove): `rocketnet_client_slots_release` with a
  plain clear removed leaves the user data in a free slot;
  `test_secrets_wiped_user_data_after_client_leaves` fails that way.
- **No slots destroy wipe** (Add, Destroy): a slot still assigned at
  `rocketnet_client_slots_destroy` is freed with its user data;
  `test_secrets_wiped_user_data_at_slots_and_map_destroy` fails that way.
- **Wipe every slot** (Destroy straight after create, TLC's trace at depth 2): a `rocketnet_client_slots_destroy` that
  zeroed every slot's user data, connected or not, writes the unassigned
  slots' pages, which the run never wrote. The code's loop wipes only
  `connected[i]` slots.
- **Ring kept on leave** (Add, Remove with the deferred burst, BurstLeaves):
  without the `sodium_memzero` of `disconnect_burst_keys[disconnect_burst_head]`
  where a complete burst leaves the head, the removed mapping's key stays in
  the ring after its burst is gone.
- **Ring not wiped at destroy** (Add, Remove with the deferred burst,
  Destroy): without the `sodium_memzero` of the whole key array in
  `rocketnet_server_transport_destroy`, a burst still waiting at destroy
  frees its key unwiped.

`test_unit_secrets_wiped.cpp` checks the code side: destroy leaves every
secret span zero and writes at most a handful of bytes of any allocation, and
each check has a reversed witness that fails it.
