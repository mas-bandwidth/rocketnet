# RocketNet examples

A feature-focused series: one plain C99 file per example, each showing one
feature with the least code that shows it, using only `rocketnet.h` and the
static development library. Each page lives in `docs/examples/`.

| Example | Shows | Page |
| --- | --- | --- |
| [hello](hello.c) | init, a server and a client over `loopback`, one reliable-ordered message, disconnect, reconnect with a second token | [hello.md](../docs/examples/hello.md) |
| [client_server_server](client_server_server.c), [client_server_client](client_server_client.c) | two processes over real UDP with production Ed25519/X25519 keys, a matchmaker stub, and a signed connect token | [client_server.md](../docs/examples/client_server.md) |
| [reliability](reliability.c) | all eight reliability modes side by side under simulated loss and jitter, showing delivery, order, and receipt outcomes | [reliability.md](../docs/examples/reliability.md) |
| [channels](channels.c) | independent channels so a stalled reliable-ordered chat stream never blocks time-critical position updates | [channels.md](../docs/examples/channels.md) |
| [ack_receipts](ack_receipts.c) | `ROCKETNET_UNRELIABLE_WITH_ACK_RECEIPT` and `rocketnet_client_receipts` reporting `ACKED` or `LOSS` per snapshot | [ack_receipts.md](../docs/examples/ack_receipts.md) |
| [big_messages](big_messages.c) | a 200 KB reliable message fragmented and reassembled, plus an oversize unreliable send promoted to reliable | [big_messages.md](../docs/examples/big_messages.md) |
| [budgets](budgets.c) | per-connection send budgets in kbps, runtime `rocketnet_client_set_send_kbps`, reliable waiting, and unreliable over-budget drops | [budgets.md](../docs/examples/budgets.md) |
| [config_env](config_env.c) | configuration precedence (`DEFAULT < STRUCT < ENV`) and loud refusal of an out-of-range environment override | [config_env.md](../docs/examples/config_env.md) |
| [transports](transports.c) | server transport selection (`auto`, a single-transport pin, a candidate list, and refusal of an unknown transport) | [transports.md](../docs/examples/transports.md) |
| [stats](stats.c) | connection state, RTT, jitter, packet loss, bandwidth, and counters queried from both client and server | [stats.md](../docs/examples/stats.md) |
| [game_loop](game_loop.c) | a 60 Hz fixed-step loop driving one server and N clients, measuring frame order and per-frame client cost | [game_loop.md](../docs/examples/game_loop.md) |

An example that needs two processes is split into `<name>_server.c` and
`<name>_client.c`, built as `example_<name>_server` and
`example_<name>_client` and run server first. Every other example is one
program and runs alone.

Build everything as usual; `examples/CMakeLists.txt` builds each
`examples/*.c` as `example_<name>` into `bin/` (`bin\Release\` on Windows).
Run the series from the repository root:

```sh
python3 tools/check_examples.py        # includes and public API only
python3 tools/run_examples.py --build build
```

`tools/check_examples.py` refuses an include that is not `rocketnet.h` or a C
standard header, and a `rocketnet_` name that `sdk/rocketnet.h` does not
declare. `tools/run_examples.py` runs every example under a 120-second
timeout and requires `PASS` as the last output line. Examples are the
functional tier: they use the real transport and the wall clock, so they are
not part of `./bin/test`.
