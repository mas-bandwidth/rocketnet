# The instance heap: who allocates, when, and from where

`RNInstanceHeap.tla` models the memory ownership of a public client or server
over its lifecycle (`sdk/rocketnet_api.cpp`; the heap itself is
`sdk/rocketnet_allocator.h`, "the instance heap"). Create takes one block from
the installed allocator, holding the object, its connection memory and a
heap sized from the transport plan of the largest candidate the ladder can
grant. Every allocation after that comes from the heap, on the caller's
thread.

## Actions

- `Create`: the one block from the installed allocator.
- `Connect` (a server's start): refused while the retired transport's worker
  runs; otherwise the retired transport goes back to the heap first, and a
  new one is built.
- `Allocate`, `Built`: one allocation of the new transport from the heap; a
  refusal abandons the connect, giving back what it built.
- `Disconnect` (stop): the transport retires, its worker asked to stop.
- `WorkerStops`: the worker's completion (environment).
- `Destroy`: the retired transport back to the heap, then the block back to
  the installed allocator.

The heap is abstracted to units: a transport generation is `Allocations`
units, and the heap holds one generation's plan.

## Properties

| name | kind | meaning |
|---|---|---|
| `NoRefusal` | invariant | a plan-sized heap never refuses |
| `WithinPlan` | invariant | the heap never holds more than one generation's plan |
| `InstalledOnlyAtCreateDestroy` | invariant | the installed allocator is called at create and destroy only |
| `NothingLiveAtFree` | invariant | the block goes back with nothing live in its heap |
| `FreedOnlyStopped` | invariant | a transport goes back only after its worker stopped |
| `ConnectBuilt` | liveness | a connect is built or refused, never left building |

## Cases

| config | expected |
|---|---|
| `MCRNInstanceHeap.cfg` | pass |
| `MCRNInstanceHeapBrokenAllocBeforeFree.cfg` | `NoRefusal`: connect builds before freeing the retired transport |
| `MCRNInstanceHeapBrokenConnectUsesUser.cfg` | `InstalledOnlyAtCreateDestroy`: connect allocates from the installed allocator |
| `MCRNInstanceHeapBrokenDestroyLeaks.cfg` | `NothingLiveAtFree`: destroy frees the block with a transport live in the heap |
| `MCRNInstanceHeapBrokenFreeWhileRunning.cfg` | `FreedOnlyStopped`: connect frees the retired transport while its worker runs |

Each reversed witness against the code: `rocketnet_client_connect` returns
`CLIENT_NOT_DISCONNECTED` while the retired worker runs, and calls
`rn_client_end_connection` (which destroys the retired transport) before
`rocketnet_message_client_create_resolved`; `rocketnet_server_start` calls
`rn_server_finish_stop` before it creates the next transport; both destroys
free the retired transport, then check the heap holds nothing, then free the
block. The connect-uses-user witness is a connect or start that allocates
from the installed allocator, which
`test_instance_heap_client_connect_allocates_nothing_after_create` and
`test_instance_heap_server_start_allocates_nothing_after_create` measure
under a counting allocator.
