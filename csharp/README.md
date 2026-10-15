# rocketnet C# binding

P/Invoke binding over the public C surface in `sdk/rocketnet.h`, as one static
class `RocketNet`: raw statics mirroring the C functions one to one, blittable
structs matching the C layouts, and thin span helpers over the pointer-shaped
calls (checked packet payload spans, payload batch views, `CountersSpan`). Targets net8.0,
zero package dependencies. `AllowUnsafeBlocks` is on for the fixed-size buffers
and payload spans.

Connect-token writers and client constructors accept exactly `ConnectTokenBytes`
bytes. Array overloads reject null with `ArgumentNullException`; wrong-sized
arrays and spans throw `ArgumentException` before a native call. The span writer
pins writable storage; constructors pin a read-only token for the synchronous
native call. Native token imports are private. Existing array call sites remain
source compatible.

The binding loads the shared library `librocketnet` (cmake target
`rocketnet_shared`) by name: the runtime probes the executable's directory and
the system paths, and `ROCKETNET_LIBRARY_PATH` — a file, or a directory holding
the library — overrides. An explicit missing or unloadable path fails; it never
silently falls back to a different library.

Before resolving the first native call, the binding validates the SDK ABI
revision and all seven struct sizes, field counts, offsets and widths against
compiler-derived native metadata. `ValidateAbi()` runs this check explicitly
without initializing RocketNet. A mismatch or a library predating introspection
throws `BadImageFormatException` before any config, stats or packet struct is
passed to native code. Update the native library and binding together when
adopting this change. Existing native struct layouts and packet encoding are
unchanged. Validation happens once at loading, with no per-packet checks added.
Invalid managed token buffers still fail before loading the library.

Build the native library, then run the smoke test, from the repo root:

    cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target rocketnet_shared -j
    ROCKETNET_LIBRARY_PATH=bin dotnet run --project csharp/smoke

The smoke test is the binding's proof: loopback connect over the portable
backend, byte-exact payload exchange both directions through the
acquire/commit path and the convenience sends, a fragmented payload --
requested above the wire's capacity at StartPacket -- crossing whole with
its one packet id acknowledged, a deliberate server-side
disconnect observed by the client, and the negative controls — a corrupted
connect token refused at create, an oversize finish dropped, and a corrupted
sealed block refused by the server's signature check. CI runs it on Linux, macOS and Windows.

Packet payload access now requires its owner: migrate `PayloadSpan(in context)`
to `ClientTransportPacketPayloadSpan(client, in context)` or
`ServerTransportPacketPayloadSpan(server, in context)`. The unchecked helper is
removed. The new accessor validates the lease before exposing memory; stale,
modified, consumed and wrong-owner contexts yield an empty span. All payload
views still expire at finish, abort, update, flush or destroy and must not race
those operations. Native/managed struct layout remains unchanged.

Frame sync is now a safe handle. `FrameSyncCreate` returns a `FrameSyncHandle`
(dispose it once, or call `FrameSyncDestroy` with it); `FrameSyncWait` and
`FrameSyncGetStats` take that handle. A second destroy, or a wait or stats read
after destroy, throws `ObjectDisposedException` in managed code before the
native handle is touched. Migrate `IntPtr frameSync = FrameSyncCreate(...)` to
`FrameSyncHandle frameSync = FrameSyncCreate(...)` and compare `.IsInvalid`
instead of `== IntPtr.Zero`. The type change is intentional and happens before
1.0.

Received payloads are now checked views. `ClientTransportPayloadSpan`,
`ServerTransportPayloadSpan` and both payload-batch indexers return
`ReceivedPayload`, a `ref struct` whose `Span`, `Length`, `IsEmpty` and indexer
each check the owner transport's update epoch captured at creation. Reading one
after the next update throws `StalePayloadException` instead of handing back
memory the library has reused. Migrate `ReadOnlySpan<byte> payload = batch[i]`
to `ReadOnlySpan<byte> payload = batch[i].Span`, and the single-payload
accessors likewise (`.Span`). The step before an update is still to consume the
payload; the check turns a stale read into an exception rather than silent
corruption. Native/managed layout is unchanged. The acked-packet accessors are
not payload accessors and keep their existing lifetime.

`CounterCount()` reports the loaded library's counter count; `CountersSpan` uses
it instead of a compiled array length. `CounterValue` is bounds checked. Read
counters outside update/fill. Native and managed builds remain a matched pair.
ABI revision checks reject incompatibility; they do not extend payload-view
lifetimes or add automatic transport ownership.

CI builds intentionally incompatible libraries and proves that revision, size,
field-count, offset and width mismatches all refuse before a struct-writing
entry point. A legacy library with no ABI queries also refuses; its config
function aborts if the guard fails. Run these checks with
`python3 tools/check_managed_abi.py bin` after building both ABI fixture targets
and the Release smoke project.

## Managed/native combinations

| Managed binding | Native library | Current behavior |
| --- | --- | --- |
| Current binding | ABI revision 1 with every queried layout matching | Validates once, then runs the transport API; exercised by the three-platform smoke. |
| Current binding | No ABI introspection exports | Refuses at load with `BadImageFormatException`, before passing a struct to native code. |
| Current binding | Different revision, type size, field count, field offset or field width | Refuses at load with `BadImageFormatException`; each mismatch has an independent fixture. |
| Current binding | Explicit path is missing or unloadable | Load fails; the resolver does not choose another library. |
| Older binding without the load gate | Newer native library | That binding does not perform these checks. This matrix does not certify the combination; rebuild the binding and native library together. |

The packet wire version is independent of this SDK compatibility check. Matching
wire bytes do not establish compatible managed/native layouts. A future change
to sized structs or the revision policy requires its own compatibility decision.

For the removed ownerless helper, keep the transport handle alongside the packet
context and pass it to the new accessor:

```csharp
Span<byte> payload = RocketNet.ServerTransportPacketPayloadSpan(server, in context);
// Fill the view before finishing/aborting the packet or advancing its owner.
```

An already-returned span is borrowed memory: validation cannot revoke it when
another call finishes the packet. Do not retain it past the documented lifetime.
The ownerless helper is not restored by this migration note. Any compatibility
adapter for an existing consumer needs that consumer's usage and lifetime
requirements first; an obsolete annotation alone cannot validate ownership.
