# RocketNet examples

A feature-focused series: one plain C99 file per example, each showing one
feature with the least code that shows it, using only `rocketnet.h` and the
static development library. Each page lives in `docs/examples/`.

| Example | Shows | Page |
| --- | --- | --- |
| [hello](hello.c) | init, a server and a client over `loopback`, one reliable-ordered message, disconnect, reconnect with a second token | [hello.md](../docs/examples/hello.md) |

An example that needs two processes is split into `<name>_server.c` and
`<name>_client.c`, built as `example_<name>_server` and
`example_<name>_client` and run server first. Such a pair takes its port from
`--port <n>`, else the environment variable `ROCKETNET_EXAMPLE_PORT`, else the
default `40000`. Every other example is one program, binds `127.0.0.1:0` so
the operating system picks a free port, and runs alone.

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
timeout and requires `PASS` as the last output line. It runs each pair in its
own temporary working directory on a free UDP port it picks and passes through
`ROCKETNET_EXAMPLE_PORT`, retrying on a new port at most three times when the
server cannot bind, so two runs at once on one machine do not collide. Examples are the
functional tier: they use the real transport and the wall clock, so they are
not part of `./bin/test`.
