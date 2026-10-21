# RocketNet v1.0.0-preview-1

> Release draft: publish after the final API review and unit and functional
> tests are green on Windows, macOS and Linux.

Low-level client/server UDP transport library.

This preview finalizes the public C API. Unit and functional tests pass on
Windows, macOS and Linux. Longer soak, load and performance testing remains
part of the work toward 1.0.0.

## The public API

Include `rocketnet.h` from C or C++ and link RocketNet. Initialize once with
`rocketnet_init`, create the endpoints, and destroy them before
`rocketnet_term`.

Fill each config with `rocketnet_client_default_config` or
`rocketnet_server_default_config` before changing fields. These inline helpers
set the first field, `config_bytes`, to the size of the caller's struct.
A zeroed config is refused with `ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED`.
There is one default config for each endpoint, used in development and
production. Create allocates through the configured allocator;
`rocketnet_client_required_memory` and `rocketnet_server_required_memory`
report the endpoint block sizes.

The frame loop is update, receive, simulate, write outgoing messages, then
send packets. Update returns `void`; read connection state and stats to see
what happened. Start a message, write its bytes in place, and finish or abort
it. Server finish and abort act on the pending message and take no destination.
A refused finish aborts that pending message so the next start can proceed.

Client failure states are negative, `DISCONNECTED` is zero, and `CONNECTED`
is the highest state. Read the reason a client connection ended from its
state. Client indexes are `int`; `rocketnet_client_index` returns `-1` when
not connected. The server send destination is `uint64_t`, with the high bit
reserved for future flow IDs; those IDs are refused in this preview.

Read the current stats with `rocketnet_client_stats`, `rocketnet_server_stats` and
`rocketnet_server_client_stats`, each filling its matching struct. Client
and server stats include the process-wide API misuse and thread-priority
refusal counters.

A send budget of `0` selects the default. `ROCKETNET_SEND_KBPS_UNLIMITED`
selects no send budget; negative rates are refused. Configure logging
with `rocketnet_log_level`, `rocketnet_quiet` and `rocketnet_log_function`.
Use `rocketnet_assert_function` for assertion handling and install a custom
allocator with `rocketnet_allocator` before initialization.

The network simulator is a development feature. A development library has
`rocketnet_client_network_simulator` and `rocketnet_server_network_simulator`,
which add latency, jitter, packet loss and duplication to outgoing packets,
and the `ROCKETNET_DEV_CLIENT_*` and `ROCKETNET_DEV_SERVER_*` environment
variables that set the same values at create. The config structs are the same
in development and production builds; a production library has neither the
calls nor the variables.

## Starting a server

Supply your backend's Ed25519 public key through
`ROCKETNET_SERVER_BACKEND_PUBLIC_KEY`, base64-encoded, and your nonzero
protocol ID through `ROCKETNET_SERVER_PROTOCOL_ID`, in decimal. You can also
set these fields in the config; the environment overrides them. Keep the
backend's signing secret on the backend.

Initialize the server config and call `rocketnet_server_create` with the
server address and a status out-parameter. Check both the returned pointer
and status. Create generates a fresh server keypair and leaves the server
stopped. Call `rocketnet_server_start` and check its status before accepting
clients.

Read the public key with `rocketnet_server_public_key`. Once started, read
its addresses with `rocketnet_server_ipv4_address` and
`rocketnet_server_ipv6_address`, using buffers of
`ROCKETNET_MAX_ADDRESS_STRING_BYTES`. A getter returns false when the server
does not serve that family.

Register the public key and served addresses with your backend over your
own authenticated channel. The backend issues connect tokens for that key
and those addresses. The C API does not generate production connect tokens
or expose key-generation calls. A new server object has a new keypair;
register its public key before issuing tokens for it.

## Connecting in development

Build against the development library with `ROCKETNET_DEVELOPMENT` enabled.
Use the same default config helpers. Set the server's backend public key to
the published development key `ROCKETNET_DEV_BACKEND_PUBLIC_KEY` from
`rocketnet.h`
and choose a nonzero protocol ID shared with the tokens. Start the server
and read its generated public key and served addresses as above.

Call `rocketnet_create_dev_connect_token` with the IPv4 address, IPv6 address,
server public key, lifetime in seconds, idle timeout in seconds, client ID,
protocol ID, current Unix timestamp, optional user data, and output string.
Pass `NULL` for an address family the server does not serve, but supply at
least one family. Use a `char` buffer of
`ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES` for the result and check the status.

Pass the resulting opaque base64 string directly to
`rocketnet_client_connect`. The client takes a `const char *`; it does not
need to decode the token. Drive update and send packets to complete the
connection. Production clients receive the same kind of string from the
backend. A production server refuses the published development key.

Known issues: see [ROADMAP.md, Known issues](../ROADMAP.md#known-issues-addressed-in-the-final-deliverable).

See [BUILDING.md](../BUILDING.md) to build and link, and
[sdk/rocketnet.h](../sdk/rocketnet.h) for the full API contract.
