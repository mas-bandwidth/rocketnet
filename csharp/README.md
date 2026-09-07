# rocketnet C# binding

P/Invoke binding over the public C surface in `sdk/rocketnet.h`, as one static
class `RocketNet`: raw statics mirroring the C functions one to one, blittable
structs matching the C layouts, and thin span helpers over the pointer-shaped
calls (`PayloadSpan`, the payload batch views, `CountersSpan`). Targets net8.0,
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
the library — overrides.

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
sealed block refused by the server's signature check. CI runs it on ubuntu
and macos.
