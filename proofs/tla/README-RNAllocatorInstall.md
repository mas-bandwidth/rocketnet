# The installed allocator with no lock

`RNAllocatorInstall.tla` models how `rocketnet_malloc`, `rocketnet_free` and
an allocator install (`rocketnet_init`, `rocketnet_allocator_ex`) share the
installed allocator pair and its live count without a lock
(`sdk/rocketnet.cpp`). Every step is one sequentially consistent operation,
as in the code.

- A call counts itself (`CallCount`), reads the pair pointer (`CallRead`):
  on the installing sentinel it uncounts and starts again, otherwise it
  copies the pair and allocates (`CallAllocate`; a failed allocation
  uncounts).
- A free releases through the header's release and uncounts (`CallFree`).
- An install (one at a time, under the lifecycle mutex) publishes the
  sentinel (`InstallBegin`), reads the count (`InstallCheck`), and puts the
  old pair back and refuses with `LIVE_ALLOCATIONS`, or publishes the new
  pair (`InstallPublish`).

| name | meaning |
|---|---|
| `ReleasedThroughItsPair` | a live allocation keeps the release of the pair it came from, and that pair stays published |
| `CountCoversCalls` | the count covers every call from its count to its uncount |
| `InstallOnlyWhenIdle` | install never publishes while an allocation of the old pair is live or in flight |

| config | expected |
|---|---|
| `MCRNAllocatorInstall.cfg` | pass (2 calls, 2 installs) |
| `MCRNAllocatorInstallBrokenNoSentinel.cfg` | `InstallOnlyWhenIdle` |
| `MCRNAllocatorInstallBrokenNoSentinelRelease.cfg` | `ReleasedThroughItsPair` |

The reversed witnesses are the install with no sentinel:
install reads a zero count, a call counts itself and reads the old pair,
and install publishes the new one with that call in flight. The trace was
checked by hand against that code (`rn_allocator_install_locked` loading the
count before its store, `rocketnet_malloc` loading the pair after its
`fetch_add`).
