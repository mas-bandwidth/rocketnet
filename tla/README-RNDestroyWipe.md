# RNDestroyWipe

What server destroy writes, in `sdk/rocketnet_api.cpp`
(`rocketnet_server_destroy`), `sdk/rocketnet_server_transport.cpp`
(`rocketnet_server_transport_destroy`) and `sdk/rocketnet_encryption_map.cpp`
(`rocketnet_encryption_map_remove`, `rocketnet_encryption_map_destroy`) and
`sdk/rocketnet_client_slots.cpp` (`rocketnet_client_slots_release`,
`rocketnet_client_slots_destroy`). The
client is the same shape: `rocketnet_client_protocol_wipe_secrets` at the
connection reset and at the client transport's destroy.

Destroy used to wipe the whole pre-allocated block once a start had used it,
touching every page of a plan that reaches gigabytes: at 1000 clients the
example soak's shutdown peak was 29.4 GB against an 18.4 GB run, and the
Linux soak was OOM-killed. Destroy now wipes exactly the secrets and frees the
block as it is; the OS zero-fills a page before another process gets it.

- **Use(p)**: a connection writes a page of the block; only use touches a page.
- **Add(m)**: a mapping is written whole, its session keys, nonce and
  connect-token user data with it, and the client slot takes a copy of the
  user data. The user data comes out of the token's private part and can
  carry the game's secrets.
- **Remove(m)**: the mapping's session keys, nonce and user data are wiped and
  it leaves live; the slot's release wipes its copy.
- **Destroy**: the server secret key and both cookie keys are wiped, the
  secrets of every live mapping, and every slot's user data (an array create
  zeroed, so no new page); nothing else is written.

## Invariants

- **`SecretsZeroAtFree`**: when the block is freed, no secret is set.
- **`ShutdownPeakWithinRun`**: destroy touches no page the run did not, so the
  shutdown's resident peak is never above the run's.
- **`NoSecretOutsideLive`**: while running, only a live mapping and its slot
  hold secrets.
- **`TypeOK`**.

## Cases

| config | constants | expected |
|---|---|---|
| `MCRNDestroyWipe.cfg` | three pages, two mappings, every rule on | pass |
| `MCRNDestroyWipeBrokenWholeBlock.cfg` | `WipeWholeBlock = TRUE` | `ShutdownPeakWithinRun` violated |
| `MCRNDestroyWipeBrokenNoRemoveWipe.cfg` | `WipeOnRemove = FALSE` | `NoSecretOutsideLive` violated |
| `MCRNDestroyWipeBrokenSkipLive.cfg` | `WipeLiveAtDestroy = FALSE` | `SecretsZeroAtFree` violated |
| `MCRNDestroyWipeBrokenNoReleaseWipe.cfg` | `WipeSlotOnRelease = FALSE` | `NoSecretOutsideLive` violated |
| `MCRNDestroyWipeBrokenNoSlotsDestroyWipe.cfg` | `WipeSlotsAtDestroy = FALSE` | `SecretsZeroAtFree` violated |

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

`test_unit_secrets_wiped.cpp` checks the code side: destroy leaves every
secret span zero and writes at most a handful of bytes of any allocation, and
each check has a reversed witness that fails it.
