# RocketNet v1.1.5

Changes compared with the `v1.1.4` tag. A point release: no configuration
field, export, status value, packet layout or protocol version changes, and
v1.0.0, v1.1.0, v1.1.1, v1.1.2, v1.1.3, v1.1.4 and v1.1.5 endpoints
interoperate in both directions.

- Tag: `v1.1.5`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.5).

## Fixes since v1.1.4

- An aborted message now gives its send room back at once, even when a large message is still queued ahead of it. Before, `rocketnet_client_abort_message` and `rocketnet_server_abort_message` released the message, but its room in the send buffer came back only after every older queued message had gone, so a later message could be refused with room that should have been free. The same code is in every release since v1.0.0.
- A refused message start (`rocketnet_client_start_message` or `rocketnet_server_start_message` returning NULL because the send side is full) now leaves an open unreliable message and the connection's statistics as they were. Before, the refusal first closed the open unreliable message and sent it, and counted that send. The open message now goes in the next flush or update, as it would have with no refused start.

## Tools

- `scripts/rocketnet-provision` has a new `repin` mode for NICs whose XDP attach rebuilds the interrupt vectors (ixgbe among them): run it once the server is up, and it pins each queue's interrupt back to the isolated CPU of the XDP worker that polls it, without touching the running server. Provision now sets the queue count (R7) before it pins interrupts (R4), so a port left with many channels provisions on the first run. DEPLOYING.md describes both.
- The XDP packet blaster takes `--threads N` to send from several threads at once.
- The load tool fails its verdict when a client had every send refused in one direction for the whole run, and names those clients (LOAD.md).
- New tests run network outages in each direction and both, shorter and longer than the timeout, over the in-memory link in every commit's tests and nightly, and over real sockets in the nightly soak; they also cover many clients sending at once, each client's share of the send room, and slot reuse. A nightly cross-version check connects this release with the previous release and with v1.0.0, each as client and as server, through message traffic, link weather, outages, a killed client and the connection lifecycle (TESTING.md). Test, model, tool and CI changes only.
