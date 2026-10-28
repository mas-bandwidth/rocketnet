# Transports

How a server picks its transport, and how to read the choice: automatic
selection, a pin, and a candidate list. Each server that starts is proved with
a client exchange over the loopback interface, `127.0.0.1`.
## What it shows

Three kinds of `transport` setting, each created in turn on a server:
- **auto** (`NULL`): the platform's recommended ladder from `sdk/rocketnet_drivers.inc`. A Linux server tries
  `xdp`, then `gso`, then `io_uring`, then `portable`; macOS tries `msgx`, then `portable`;
  Windows tries `iocp`, then `rio`, then `portable` in production builds (`portable`, then `iocp`, then `rio` in development builds). Clients on `auto` try `gso`, then `portable` on Linux; `msgx`, then `portable` on macOS; and `iocp`, then `portable` on Windows. The first transport that starts wins.
- **a pin**: one name. The server gets exactly that transport or start fails
  loudly; there is no fallback.
- **a list**: comma-separated names tried left to right. The first one granted
  wins, and the library logs each refusal, so `xdp,gso,portable` lands on the
  best transport this machine and this user can actually have. A known name not offered on this platform or endpoint is skipped with a log line; if every name in the list is skipped, the endpoint falls back to its `auto` order with a warning.
The example then pins a name no transport has. The library refuses it with one
error line listing the valid names, and the program prints that the refusal was
expected. Every successful server is proved with a client that connects, sends
one reliable-ordered message (guarded by checking the non-`NULL` pointer from
`rocketnet_client_can_send_message`, `rocketnet_client_start_message` before calling `rocketnet_client_finish_message`
inside that branch), and disconnects.
Each frame runs the calls in the order a game does: update, drain the one
receive queue until `NULL`, simulate, queue, then send last. Sending last is what makes this
frame's messages leave this frame instead of waiting a frame. See
[TRANSPORTS.md](../../TRANSPORTS.md) for the complete transport table and host
requirements.

## Run it
From the repository root, after the normal build:

```sh
./bin/example_transports
```

On Windows the binary is `bin\Release\example_transports.exe`. It finishes well
inside five seconds per exchange and prints `PASS` as its last line; any
failure prints one `FAIL: ...` line and exits 1.

## What the output means
```
warning: server transport xdp refused: a port-zero public address ...
warning: server transport xdp refused. trying the next candidate
info: server transport ladder granted gso after refusals: xdp
auto chose gso
pin chose portable
list chose gso
error: unknown transport "not-a-transport" in transport list ... (valid names: ...)
refused as promised: the pin named no transport
PASS
```

The `info:` and `error:` lines before each `chose` line are the library's own
log lines: they say which candidate was granted and why the others were
skipped or refused. The `chose` line is the public query
(`rocketnet_server_transport_name`). `PASS` means all three creates behaved as
their kind promises and every started server completed the exchange.

## Selecting a transport in a deployment

The environment overrides the config struct at create, with precedence
default < struct < environment:

`portable` is available to clients and servers in development and production
builds on every platform. Production server auto selection prefers native
transports, with portable as the final fallback.

```sh
ROCKETNET_SERVER_TRANSPORT=portable ./my_server
ROCKETNET_CLIENT_TRANSPORT=portable ./my_client
```

The value is the same candidate list, so a deployment can pin one transport or
keep the automatic ladder without rebuilding. The create log line names the
variable and the value it displaced.

## Public calls it uses

- `rocketnet_init`, `rocketnet_term`, `rocketnet_log_level`
- `rocketnet_server_default_config`, `rocketnet_server_create`,
  `rocketnet_server_start`, `rocketnet_server_stop`, `rocketnet_server_stopped`,
  `rocketnet_server_destroy`,
  `rocketnet_server_update`, `rocketnet_server_send_packets`,
  `rocketnet_server_ipv4_address`,
  `rocketnet_server_public_key`, `rocketnet_server_transport_name`,
  `rocketnet_server_receive_messages`
- `rocketnet_client_default_config`, `rocketnet_client_create`,
  `rocketnet_client_connect`, `rocketnet_client_disconnected`,
  `rocketnet_client_destroy`,
  `rocketnet_client_update`, `rocketnet_client_send_packets`,
  `rocketnet_client_disconnect`, `rocketnet_client_state`,
  `rocketnet_client_start_message`, `rocketnet_client_finish_message`,
  `rocketnet_client_receive_messages`
- `rocketnet_create_dev_connect_token` and
  `ROCKETNET_DEV_BACKEND_PUBLIC_KEY` (development library only)
- `rocketnet_status_name`, `rocketnet_time`, `rocketnet_sleep`
