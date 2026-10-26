# Budgets

The send budget is how many kilobits per second of wire bytes a connection may
send, per connection: the client's budget for client-to-server traffic, the
server's per-client budget for server-to-client traffic. This example shows
what a budget does when it is exceeded.

## What it shows

1. Print the client send budget default (`ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS`)
   and the server per-client send budget default
   (`ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS`).
2. Set the client's send budget lower in its config at create, then change it
   again at run time with `rocketnet_client_set_send_kbps`.
3. Queue a burst of unreliable messages far above the budget. Unreliable
   messages over budget are dropped and counted. The example calls
   `rocketnet_client_stats` and prints
   `rocketnet_client_stats_t.messages_dropped_over_budget`.
4. Queue a burst of reliable messages of the same size. Reliable messages wait
   for budget and are never dropped, and the example prints how many arrived.

The budget governs the wire, never the queue: reliable messages wait for
budget and arrive, unreliable ones over budget are dropped and counted.

An environment variable can override a budget at create:
`ROCKETNET_CLIENT_SEND_KBPS` for the client,
`ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS` for the server. The runtime
set-send-kbps call wins over both.

## Run it

From the repository root, after the normal build:

```sh
./bin/example_budgets
```

On Windows the binary is `bin\Release\example_budgets.exe`. The program finishes
inside sixty seconds and prints `PASS` as its last line; any failure prints one
`FAIL: ...` line and exits 1.

## What the output means

The program prints the two header defaults, the configured client rate, the
numeric status from `rocketnet_client_set_send_kbps`, the final
`messages_dropped_over_budget` value, and the reliable arrival count. A successful runtime
change prints status `0`, the numeric value of `ROCKETNET_STATUS_OK`. The drop
count is run-dependent but must be nonzero, while the reliable arrival line
must report every queued reliable message.

## Public calls it uses

- `rocketnet_init`, `rocketnet_term`
- `rocketnet_server_default_config`, `rocketnet_server_create`,
  `rocketnet_server_start`, `rocketnet_server_destroy`,
  `rocketnet_server_update`, `rocketnet_server_send_packets`,
  `rocketnet_server_ipv4_address`,
  `rocketnet_server_public_key`, `rocketnet_server_receive_messages`
- `rocketnet_client_default_config`, `rocketnet_client_create`,
  `rocketnet_client_connect`, `rocketnet_client_destroy`,
  `rocketnet_client_update`, `rocketnet_client_send_packets`,
  `rocketnet_client_state`, `rocketnet_client_index`,
  `rocketnet_client_can_send_message`, `rocketnet_client_start_message`, `rocketnet_client_finish_message`,
  `rocketnet_client_receive_messages`, `rocketnet_client_set_send_kbps`,
  `rocketnet_client_stats`
- `rocketnet_create_dev_connect_token` and
  `ROCKETNET_DEV_BACKEND_PUBLIC_KEY` (development library only)
- `rocketnet_status_name`, `rocketnet_time`, `rocketnet_sleep`
