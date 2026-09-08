# Native transport drivers

The public transport API in `rocketnet.h` is independent of the backend. C#
uses that API, and will use the future client/server API above it. Native
drivers implement `rocketnet_driver.h`; they do not add managed bindings or
wire-format rules.

`rocketnet_drivers.inc` is the built-in registry. Each row supplies a name,
bound-port discovery property, backend record, platform mask, and server/client
preference ranks. Lower nonzero ranks come first in `auto`; zero means that
endpoint is unsupported. IDs, names, diagnostics and platform availability are
derived from these rows. Existing IDs and preference orders are preserved.

The backend record points to immutable server and/or client operations. The
transport retains the selected operations and opaque instance for its lifetime.
Factories, resources and platform decisions stay in the driver layer. The
transport branches on generic capabilities, never a specific backend identity.

## Server capabilities and ownership

Two I/O shapes preserve the existing implementations:

- **Buffered:** the transport supplies its common control, payload and receive
  buffers. `send_batch` and `receive_batch` borrow that storage only for the call.
  This is the portable/msgx shape. It does not require a backend-owned pool.
- **Staged:** the driver supplies frame storage and a complete receive batch.
  XDP, GSO and IOCP use this shape, with their own pools and worker arrangements.
  The transport does not allocate, recycle or schedule those backend frames.

The staged callbacks are start, finish, abort, flush and receive. Start returns
a `ROCKETNET_FRAME_STRIDE` root; the wire starts at `ROCKETNET_FRAME_WIRE_OFFSET`.
The caller-visible payload alignment must remain valid. Receive returns at most
`ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS` datagrams, each within the receive
ceiling, with normalized source addresses. Its data remains valid until the
next receive or destruction, including while workers accept further packets.

The flags express the differences the transport needs:

| Flag | Meaning |
| --- | --- |
| `STAGED` | Use driver-owned frames and receive views instead of borrowed transport buffers. |
| `FLUSH_ON_UPDATE` | Flush staged control traffic at the end of update. Buffered control traffic always completes its synchronous send there. |
| `FINISH_FINAL` | A finished frame cannot be withdrawn. Otherwise abort may withdraw it before flush. Unfinished reservations can always be aborted. |
| `BINDS_PUBLIC_ADDRESS` | The factory binds the advertised address rather than the transport's ordinary bind address. |

A flush means submission, not necessarily completion or reusable storage. IOCP
keeps operation buffers until completion retirement. The backend owns that
decision; it must never hand a still-owned frame to another producer.

Start/finish/abort may run on concurrent fill threads, with one producer per
frame. Join them before update, receive, flush, stop or destroy. Flush is called
once for a staging generation; this preserves XDP's buffer-flip behavior. None
of these rules sets a frame, packet-send or ACK rate.

## Creation and shutdown

The factory borrows resolved configuration for the call and retains only what
its instance needs. It owns partial-construction cleanup. A failed candidate's
tentative bind address cannot constrain the next candidate. A discovering
factory reports its actual port on success. Existing XDP preflight and the
commit barrier remain in its adapter/backend.

Server stop is an idempotent stop/poll callback: false means work remains; true
allows the transport to report stopped. It must not wait for outstanding I/O
inside update. Destroy handles partial setup and a created instance that never
received an update, and drains/joins its owned operations before releasing
their storage. GSO/XDP preserve their existing stop/destruction behavior; IOCP
uses its cancellation and completion-drain implementation.

Clients use create/destroy/send/receive callbacks. The existing transport
receive thread and buffers remain shared infrastructure. Receive can run
concurrently with sends; both calls borrow their buffers synchronously. Destroy
runs after that thread has joined and must release any additional backend-owned
work. Client fast-path I/O is not required by this interface.

## Adding a backend without editing the transport

For an in-tree driver, define a `const rocketnet_driver_backend_t`, add its row
to `rocketnet_drivers.inc`, and add its source to the build. It may support one
endpoint or both. There are no transport switches, members or platform guards
to extend.

An embedding project can also supply its own registry rows and implementation:

```sh
cmake -S . -B build \
  -DROCKETNET_DRIVER_REGISTRY_HEADER=/path/to/my_drivers.inc \
  -DROCKETNET_DRIVER_SOURCES=/path/to/my_driver.cpp
```

The `.inc` has `ROCKETNET_DRIVER(...)` rows in the same format as the built-in
file, and no include guard: it is intentionally expanded more than once. Native
registration is compiled into the SDK; this is not a dynamic plugin ABI.
Backend symbols are not public C exports. Per-driver settings and resource
validation belong in the factory; the public transport configuration is unchanged.

`test/driver_extension` is an executable example: an external row and backend
serve both ends of an encrypted handshake and payload exchange through only
the public transport API. `tools/check_driver_extension.py build` builds and
runs it independently. `test_driver` checks registry ordering, factory refusal,
address isolation, lifecycle and buffered I/O. Existing GSO/IOCP and complete
transport tests cover the real staged paths; hosted tests do not replace XDP
hardware failure tests or per-platform line-rate measurements.

Dispatch measurements and their limits are recorded in
[bench/transport_fill.md](bench/transport_fill.md).
