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
