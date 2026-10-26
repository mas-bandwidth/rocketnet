# Config environment

Shows configuration precedence: default < struct < environment, with a bad
environment value refused.

## What it shows

1. Print two defaults from `rocketnet_server_default_config`:
   `max_clients` and `max_timeout_seconds`.
2. Set them in the config struct, create a server, and print the struct
   `max_clients` (16) back.
3. Re-run itself with `ROCKETNET_SERVER_MAX_CLIENTS=5` set, create a server
   whose struct still says 16, and show the environment wins: the library logs
   the override and the server reports 5.
4. Re-run itself with `ROCKETNET_SERVER_MAX_CLIENTS=99999` (out of range for
   `1 .. ROCKETNET_MAX_CLIENTS`), and show the create is refused with
   `ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE`.

The C standard library cannot set an environment variable in-process, and the
header offers no config-read call taking an explicit environment, so the
example spawns children with `system()` and the variable set; its own
environment is never changed.

## Run it

From the repository root, after the normal build:

```sh
./bin/example_config_env
```

On Windows the binary is `bin\Release\example_config_env.exe`. It finishes in
a moment, prints `PASS` as its last line, and exits 0; any failure prints one
`FAIL: ...` line and exits 1.

## What the output means

```
default max_clients: 1000
default max_timeout_seconds: 60
struct max_clients: 16
[library: env override ... ROCKETNET_SERVER_MAX_CLIENTS=5 ...]
environment max_clients: 5
[library: ... ROCKETNET_SERVER_MAX_CLIENTS=99999 ... out of range]
bad value refused: CONFIG_OUT_OF_RANGE
PASS
```

`default` lines are the field defaults. `struct max_clients: 16` is the
struct value in force. The library's override log line then shows the
environment displacing the struct value, and `environment max_clients: 5`
confirms it won. The final library error line and `bad value refused: CONFIG_OUT_OF_RANGE` confirm the out-of-range value was refused. `PASS`
means each precedence held and the bad value was refused.

## Public calls it uses

- `rocketnet_init`, `rocketnet_term`
- `rocketnet_server_default_config`, `rocketnet_server_create`,
  `rocketnet_server_destroy`, `rocketnet_server_max_clients`
- `rocketnet_status_name`, `ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE`
- The C standard library: `getenv`, `system`, `strcmp`, `memcpy`, `snprintf`, `printf`

The full field list lives in `sdk/rocketnet.h`.

## Disconnect notification work

`disconnect_packets_per_update` has the same precedence rules. Its environment
name is `ROCKETNET_SERVER_DISCONNECT_PACKETS_PER_UPDATE`, its default is 64,
and its valid range is 1 through 1024. It is a whole-server packet allowance,
not a per-client allowance. Setting it below the 10 redundant disconnect
copies deliberately spreads those best-effort notifications across updates;
the server still closes and frees the local session immediately. When the
bounded deferred ring is full, older work is retained and the new redundant
copies may be dropped rather than exceeding the allowance. Zero and values
above 1024 are refused with `ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE`; zero does
not mean unlimited.
