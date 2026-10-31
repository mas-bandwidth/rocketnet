# The installed allocator with no lock

`RNAllocatorInstall.tla` models how `rocketnet_malloc`, `rocketnet_free` and
an allocator install (`rocketnet_init`, `rocketnet_allocator_ex`) share the
installed allocator pair and its live count without a lock
(`sdk/rocketnet.cpp`).

The code's orderings: a call's count add, every pair load and every pair
store are `seq_cst`; the uncounts (a refused allocation, `rocketnet_free`)
are `release`; term reads the count with `acquire`. Install's safety needs
only the `seq_cst` half: the call's add and pair load against install's
sentinel store and count load, so either install sees the call counted or
the call sees the sentinel. A `release` uncount lowers the count only once
the call is over, and install's `seq_cst` load and term's `acquire` load
then see the allocator's release finished. The model interleaves whole
steps (the `seq_cst` half), treats the copy of the pair (two pointers) as
one atomic step with its read, which holds because no pair a counted call
can read is written, and does not model term.

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
| `MCRNAllocatorInstallBrokenRereadRelease.cfg` | `ReleasedThroughItsPair` |

The first two reversed witnesses are the install with no sentinel: install
reads a zero count, a call counts itself and reads the old pair, and install
publishes the new one with that call in flight (`rn_allocator_install_locked`
loading the count before any store, `rocketnet_malloc` loading the pair after
its `fetch_add`). The third is a header that takes its release from the pair
published when the allocator returns instead of the pair the call copied:
an install mid-way has the sentinel published then, so the header's release
is not the pair the block came from. Without it, `ReleasedThroughItsPair`'s
first conjunct could not fail, since the code (and the model) take the
header's release from the same copy as the allocate.
