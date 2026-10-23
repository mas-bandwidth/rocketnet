# RocketNet v1.0.0-preview-3

Changes compared with the `v1.0.0-preview-2` tag. The release commit and the
native Linux, macOS and Windows unit and functional results are recorded in the
[GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.0.0-preview-3).

## Wire compatibility

The packet encodings, connect-token encoding and protocol version are
unchanged. One server acceptance policy changed:

- A server now accepts connect tokens created up to 30 seconds ahead of its
  clock, up from 5 seconds. A token created 31 or more seconds ahead is still
  refused. Expiry order is unchanged and there is still no maximum token
  lifetime. See the token creation skew policy in
  [standard/TRANSPORT.md](../standard/TRANSPORT.md#t12) and its updated
  signed vector `t12.token-created-in-future`.

Preview-2 and preview-3 SDKs interoperate. A preview-2 server still refuses
tokens more than 5 seconds ahead of its clock.

## Portable servers in production builds

Production builds (`-DROCKETNET_DEV=OFF`) now allow the portable server
transport. Automatic selection keeps native transports first and tries
portable last: `xdp,gso,io_uring,portable` on Linux, `msgx,portable` on macOS
and `iocp,rio,portable` on Windows. An explicit `"portable"` server pin is
accepted in either build. Production builds still refuse the development keys,
development tokens and the test packet filter.

If you relied on production create or `rocketnet_server_required_memory`
refusing a portable pin, that refusal is gone. Pin a native transport by name
to require it.

## Backend examples

The Go and C# backend examples under [examples/backend](../examples/backend)
now:

- generate their own signing keypairs with `--keygen`, printed as base64
  environment assignments;
- accept server registration with `PUT /servers/{id}`, validated, and answer
  `GET /match/{clientID}` with a token for a random registered server, or
  HTTP 503 when none is registered;
- link the production library and use their own packet filter.

The end-to-end functional harness generates fresh keys, registers a live server
over HTTP, obtains a token from each backend and requires a production portable
client/server echo. Registration authentication and client restrictions are
left to the application.

## Load diagnostics

The development-only `load_split` harness captures per-connection delivery
state (sequences, reliable window, send budget and named counters) once per
second and at disconnect, with the server slot and remote address. This
instruments the open Windows-client / AF_XDP-server load case; it does not fix
it.

## Known limits

All preview-2 known limits not listed as changed above still apply; see
[the preview-2 notes](RELEASE-NOTES-v1.0.0-preview-2.md#known-limits-and-release-qualification)
and the [roadmap](../ROADMAP.md). In particular:

- The API is provisional and can change before the final release.
- Server API calls still exceed the 100 microsecond frame budget at larger
  client counts.
- The 1000-client Windows portable client against a Linux AF_XDP server still
  fails strict delivery; preview-3 adds instrumentation only.
- The uniform client buffer handoff and secrets-only destroy are not in this
  preview.
