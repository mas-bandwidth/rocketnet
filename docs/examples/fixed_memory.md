# Fixed memory

Size a server and a client before creating them, then hold each one to
exactly that many bytes for its whole lifecycle.

## What it shows

1. Fill the server and client configs the objects will be created with, and
   size them with `rocketnet_server_required_memory` and
   `rocketnet_client_required_memory`. A return of 0 means the config is
   invalid.
2. Install an allocator through `rocketnet_init`. Each create passes its
   context to every allocation for that object, so the context is the
   object's budget: the allocator charges each block to it, tracks the live
   and peak bytes, and returns `NULL` past the budget.
3. Run the lifecycle over the in-memory `loopback` transport: create, start,
   connect, a few frames with a message each way, disconnect, reconnect,
   disconnect, stop and destroy. No request is refused, and each object's
   peak is at or under its required size.
4. The negative control: the same server and client with one byte less than
   the peak each reached. Start and connect return
   `ROCKETNET_STATUS_OUT_OF_MEMORY`, and nothing is left allocated.

The required size is conservative, not exact: it includes 1 MiB of slack for
the heap a game puts over the region (its block headers, alignment and
fragmentation). A game puts its own heap, such as tlsf, over one region of
exactly that size per object and adds no margin. The example does not
implement a heap; it charges a 16-byte header per block to stand in for one.
Its client is step-driven, so every allocator call comes from one thread; a
game's heap must be thread-safe, because network threads also call the
allocator. See [MEMORY.md](../MEMORY.md) for what the size counts and what it
leaves to the operating system.

## Run it

From the repository root, after the normal build:

```sh
./bin/example_fixed_memory
```

On Windows the binary is `bin\Release\example_fixed_memory.exe`. It prints
the required and peak bytes for each object, then the refused start and
connect, and `PASS` as its last line; any failure prints one `FAIL: ...`
line and exits 1.
