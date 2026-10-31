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
   for budget while the connection remains alive, and the example prints how many arrived.

The budget governs the wire, never the queue: reliable messages wait for
budget; unreliable ones over budget are dropped and counted. The example
verifies delivery before shutdown. Reliability does not survive a disconnect,
and a reliable queue that outgrows its message storage can end the connection.

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

<a id="public-calls-it-uses"></a>

Source: [`examples/budgets.c`](../../examples/budgets.c). See the
[integration guide](../USAGE.md) for shared lifecycle, threading, and message
ownership rules.
