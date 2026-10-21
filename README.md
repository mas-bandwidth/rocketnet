# RocketNet

Low-level client/server UDP transport library.

Main frame calls aim for less than 100 microseconds in Release builds. This
target is not yet established under a 1000-client load. Create may take longer. Client disconnect and destroy wait for the
I/O thread to exit, with no promised time bound.

## Quick start

This example creates both endpoints without opening a socket,
checks the create status, and tears them down cleanly:

<!-- check_readme:example:begin -->
```c
#include "rocketnet.h"

int main( void )
{
    if ( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
        return 1;

    struct rocketnet_server_config_t server_config;
    struct rocketnet_client_config_t client_config;
    if ( rocketnet_server_default_config( &server_config ) != ROCKETNET_STATUS_OK ||
         rocketnet_client_default_config( &client_config ) != ROCKETNET_STATUS_OK )
        return 1;
    server_config.transport = "loopback";
    client_config.transport = "loopback";

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status );
    if ( !server || status != ROCKETNET_STATUS_OK )
        return 1;

    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &status );
    if ( !client || status != ROCKETNET_STATUS_OK )
        return 1;

    rocketnet_client_destroy( client );
    rocketnet_server_destroy( server );
    return rocketnet_term() == ROCKETNET_STATUS_OK ? 0 : 1;
}
```
<!-- check_readme:example:end -->

The complete frame loop is in [AGENTS.md](AGENTS.md): update with the caller's
time, drain the one `rocketnet_message_batch_t` receive queue, simulate, write
outgoing messages through start/finish, and send packets last.

## Building

See [BUILDING.md](BUILDING.md). Binaries land in `bin/`, and on Windows in
`bin\Release\`; run the test suite with `./bin/test`, or with
`bin\Release\test.exe` on Windows (PowerShell or cmd.exe).

## Documentation

- [v1.0.0-preview-1 release notes](docs/RELEASE-NOTES-v1.0.0-preview-1.md) — preview contents, platforms and known issues.

- [AGENTS.md](AGENTS.md) — guide for AI coding agents integrating RocketNet into a game.
- [USAGE.md](USAGE.md) — how to drive it: the frame, message sending and
  receiving, channels, configuration, and state transitions.
- [docs/LINUX.md](docs/LINUX.md) — the Linux server guide: the transport ladder
  (`xdp`, `gso`, `io_uring`, `portable`) and pinning, host requirements,
  provisioning, socket errors and ICMP, limits, containers, and verifying a host.
- [docs/MACOS.md](docs/MACOS.md) — building on macOS, the msgx and portable
  transports, App Nap and QoS, socket errors, the firewall prompt, lldb and
  verification on a Mac.
- [docs/PLATFORMS.md](docs/PLATFORMS.md) — supported, nightly, and unsupported platform rows and the CI workflows that prove them.
- [TRANSPORTS.md](TRANSPORTS.md) — native transport registry, capabilities, and extension interface.
- [sdk/rocketnet.h](sdk/rocketnet.h) — the public header, and the authority
  wherever a contract is written down.
- [examples/README.md](examples/README.md) — the feature-focused examples series, starting with hello.
- [standard/TRANSPORT.md](standard/TRANSPORT.md) — the transport standard: the wire format.
- [standard/MESSAGES.md](standard/MESSAGES.md) — the message standard.
- [standard/RATIONALE.md](standard/RATIONALE.md) — the rationale for both.
- [ROADMAP.md](ROADMAP.md) — the releases and the work planned.
- [CLAUDE.md](CLAUDE.md) — contributor guide.

Private repository. License: [AGPL-3.0](LICENSE).
