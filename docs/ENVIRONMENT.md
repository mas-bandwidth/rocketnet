# Environment variables

The client and server config fields that have environment forms list their
variable names beside each field in `sdk/rocketnet.h`. Their values override
the config at the documented lifecycle point, and invalid or out-of-range
values are refused rather than clamped. `docs/ABI-EVOLUTION.md` describes the
config-size rules.

`rocketnet_client_create` and `rocketnet_server_create` read every variable
once: the defaults, then the config struct, then the environment, before the
config is validated and the endpoint's memory is sized. Client connect and
server start use that resolved config and never read the environment again,
so a variable changed after create has no effect on that endpoint. The
variables below are read the same way, at create. None of them is a field of
the public config structs:

| Variable | Meaning |
| --- | --- |
| `ROCKETNET_SERVER_IPV4_ADDRESS` | Overrides the `ipv4_address` argument of `rocketnet_server_create`, read at create. Form `a.b.c.d:port`. |
| `ROCKETNET_SERVER_IPV6_ADDRESS` | Overrides the `ipv6_address` argument of `rocketnet_server_create`, read at create. Form `[v6]:port`. |
| `ROCKETNET_CLIENT_GAME_CLOCK_EPOCH` | Unix seconds at the client's game clock zero, for the connect token's time check; 0 reads the wall clock. |
| `ROCKETNET_SERVER_GAME_CLOCK_EPOCH` | Unix seconds at the server's game clock zero, for the connect token's time check; 0 reads the wall clock. |
| `ROCKETNET_CLIENT_STEP_DRIVEN` | 1 runs the client's I/O step inside update and send_packets on the caller's thread, with no network thread; 0 (the default) keeps the network thread. |
| `ROCKETNET_DEV_CLIENT_PACKET_LOSS_PERCENT` | Development only: outgoing client loss, 0 .. 100; 0 is off. |
| `ROCKETNET_DEV_SERVER_PACKET_LOSS_PERCENT` | Development only: outgoing server loss, 0 .. 100; 0 is off. |
| `ROCKETNET_DEV_CLIENT_LATENCY_MS` | Development only: outgoing client latency, 0 .. 60000 milliseconds; 0 is off. |
| `ROCKETNET_DEV_SERVER_LATENCY_MS` | Development only: outgoing server latency, 0 .. 60000 milliseconds; 0 is off. |
| `ROCKETNET_DEV_CLIENT_JITTER_MS` | Development only: outgoing client jitter, 0 .. 60000 milliseconds; 0 is off. |
| `ROCKETNET_DEV_SERVER_JITTER_MS` | Development only: outgoing server jitter, 0 .. 60000 milliseconds; 0 is off. |
| `ROCKETNET_DEV_CLIENT_DUPLICATE_PERCENT` | Development only: outgoing client duplication, 0 .. 100; 0 is off. |
| `ROCKETNET_DEV_SERVER_DUPLICATE_PERCENT` | Development only: outgoing server duplication, 0 .. 100; 0 is off. |
| `ROCKETNET_CLIENT_SIMULATED_DROP_EVERY_NTH` | Development only: drop every Nth outgoing client datagram, 2 .. 1000000; 0 is off. |
| `ROCKETNET_SERVER_SIMULATED_DROP_EVERY_NTH` | Development only: drop every Nth outgoing server datagram, 2 .. 1000000; 0 is off. |
| `ROCKETNET_CLIENT_SIMULATED_REVERSE_WINDOW` | Development only: reverse each window of W outgoing client datagrams, 2 .. 64; 0 is off. |
| `ROCKETNET_SERVER_SIMULATED_REVERSE_WINDOW` | Development only: reverse each window of W outgoing server datagrams, 2 .. 64; 0 is off. |
| `ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES` | The operator memory budget: allocator bytes the client's pre-allocation plan must fit; 0 is no budget. |
| `ROCKETNET_SERVER_MEMORY_BUDGET_BYTES` | The operator memory budget: allocator bytes a server transport candidate's pre-allocation plan must fit; 0 is no budget. |

The `ROCKETNET_DEV_` network simulator variables exist in a development
library only and are read at create; in code, the same settings are
`rocketnet_client_network_simulator` and `rocketnet_server_network_simulator`.
Tests, tools and benches set the step-driven client and the receive stamp
clock in code through `sdk/rocketnet_testing.h`.

Process-wide and platform-specific controls:

| Variable | Meaning |
| --- | --- |
| `ROCKETNET_LOG_LEVEL` | Maximum emitted log level before initialization logs: 0 .. 5 (`ROCKETNET_LOG_LEVEL_NONE` .. `ROCKETNET_LOG_LEVEL_SPAM`). A value outside that range is ignored with a warning. |
| `ROCKETNET_GATEWAY_ETHERNET_ADDRESS` | Linux XDP only: overrides gateway MAC discovery in `xx:xx:xx:xx:xx:xx` form. A malformed value refuses socket creation. |

Development token support is an API helper, not an environment variable.
Development builds expose `ROCKETNET_DEV_BACKEND_PUBLIC_KEY` and
`rocketnet_create_dev_connect_token`; see `sdk/rocketnet.h`. The public
development key must not be used for production.
