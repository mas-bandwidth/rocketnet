# Environment variables the config structs do not name

Every field of `rocketnet_client_config_t` and `rocketnet_server_config_t`
except the callbacks and `config_bytes` has an environment variable, named next
to the field in `sdk/rocketnet.h`. These variables tune what the structs do not
expose. They follow the same rules: read at create (the transport ones at
client connect and server start), logged when they override, refused loudly
when they do not parse or are out of range, and ignored with a log line when
set to the empty string.

| Variable | Meaning |
| --- | --- |
| `ROCKETNET_LOG_LEVEL` | The maximum emitted log level before `rocketnet_init` logs: 0 .. 5 (`ROCKETNET_LOG_LEVEL_NONE` .. `ROCKETNET_LOG_LEVEL_SPAM`); a value outside that range is ignored with a warning. |
| `ROCKETNET_CLIENT_GAME_CLOCK_EPOCH` | Unix seconds at the client's game clock zero, for the connect token's time check; 0 reads the wall clock. |
| `ROCKETNET_SERVER_GAME_CLOCK_EPOCH` | Unix seconds at the server's game clock zero, for the connect token's time check; 0 reads the wall clock. |
| `ROCKETNET_CLIENT_SIMULATED_DROP_EVERY_NTH` | Development only: drop every Nth outgoing client datagram, 2 .. 1000000; 0 is off. |
| `ROCKETNET_SERVER_SIMULATED_DROP_EVERY_NTH` | Development only: drop every Nth outgoing server datagram, 2 .. 1000000; 0 is off. |
| `ROCKETNET_CLIENT_SIMULATED_REVERSE_WINDOW` | Development only: reverse each window of W outgoing client datagrams, 2 .. 64; 0 is off. |
| `ROCKETNET_SERVER_SIMULATED_REVERSE_WINDOW` | Development only: reverse each window of W outgoing server datagrams, 2 .. 64; 0 is off. |
| `ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES` | The operator memory budget: allocator bytes the client's pre-allocation plan must fit; 0 is no budget. |
| `ROCKETNET_SERVER_MEMORY_BUDGET_BYTES` | The operator memory budget: allocator bytes a server transport candidate's pre-allocation plan must fit; 0 is no budget. |
| `ROCKETNET_GATEWAY_ETHERNET_ADDRESS` | Linux XDP: overrides gateway MAC discovery, `xx:xx:xx:xx:xx:xx`; a malformed value refuses the socket. Process-wide, not a config field. |
