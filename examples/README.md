# RocketNet examples

A feature-focused series: one plain C99 file per example, each showing one
feature with the least code that shows it, using only `rocketnet.h` and the
static development library. Each page lives in `docs/examples/`.

| Example | Shows | Page |
| --- | --- | --- |
| [hello](hello.c) | init, a server and a client over `loopback`, one reliable-ordered message, disconnect, reconnect with a second token | [hello.md](../docs/examples/hello.md) |
| [ack_receipts](ack_receipts.c) | message delivery receipts and notifications for unreliable and reliable message sends | [ack_receipts.md](../docs/examples/ack_receipts.md) |
| [big_messages](big_messages.c) | sending payloads larger than a single packet via multi-fragment message reassembly | [big_messages.md](../docs/examples/big_messages.md) |
| [budgets](budgets.c) | client and server send bandwidth budgets, configured rates, and over-budget packet drops | [budgets.md](../docs/examples/budgets.md) |
| [channels](channels.c) | multi-channel message multiplexing and independent channel stream delivery | [channels.md](../docs/examples/channels.md) |
| [client_server_server](client_server_server.c) / [client_server_client](client_server_client.c) | two-process client and server architecture communicating over UDP sockets | [client_server.md](../docs/examples/client_server.md) |
| [config_env](config_env.c) | configuration precedence (default < struct < environment) and environment variable overrides | [config_env.md](../docs/examples/config_env.md) |
| [game_loop](game_loop.c) | integrating RocketNet into a game tick loop with update, dispatch, and send_packets cadence | [game_loop.md](../docs/examples/game_loop.md) |
| [reliability](reliability.c) | message reliability modes: unreliable, sequenced, reliable unordered, and reliable ordered | [reliability.md](../docs/examples/reliability.md) |
| [soak](soak.c) | concurrent multi-client soak and stress load test against a single server | [soak.md](../docs/examples/soak.md) |
| [stats](stats.c) | reading connection quality stats (RTT, jitter, loss, bandwidth) and diagnostic counters | [stats.md](../docs/examples/stats.md) |
| [transports](transports.c) | transport selection, priority candidate lists, and transport fallback | [transports.md](../docs/examples/transports.md) |

An example that needs two processes is split into `<name>_server.c` and
`<name>_client.c`, built as `example_<name>_server` and
`example_<name>_client` and run server first. The server of such a pair takes
its port from `--port <n>`, else the environment variable
`ROCKETNET_EXAMPLE_PORT`, else the default `40000`. The client reads the
server's address from the key file the server writes. Every other example is one program, binds `127.0.0.1:0` so
the operating system picks a free port, and runs alone.

Build everything as usual; `examples/CMakeLists.txt` builds each
`examples/*.c` as `example_<name>` into `bin/` (`bin\Release\` on Windows).
Run the series from the repository root:

```sh
python3 tools/check_examples.py        # includes and public API only
python3 tools/run_examples.py --build build
```

`tools/check_examples.py` refuses an include that is not `rocketnet.h`,
`dev_keys.h` or an allowed C standard header, and a `rocketnet_` name that `sdk/rocketnet.h` does not
declare. `tools/run_examples.py` runs every example under a 120-second
timeout and requires `PASS` as the last output line. It runs each pair in its
own temporary working directory on a free UDP port it picks and passes through
`ROCKETNET_EXAMPLE_PORT`, retrying on a new port at most three times when the
server cannot bind, so two runs at once on one machine do not collide. Examples are the
functional tier: they use the real transport and the wall clock, so they are
not part of `./bin/test`.
