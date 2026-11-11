# RocketNet v1.1.1

Changes compared with the `v1.1.0` tag. A point release: no configuration
field, export, status value, packet layout or protocol version changes, and
v1.0.0, v1.1.0 and v1.1.1 endpoints interoperate in both directions.

- Tag: `v1.1.1`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.1).

## Fixes since v1.1.0

- A server no longer spends a send slot every update on a client held by probe pacing (a silent client with two or more reliable messages unacknowledged); such a client takes a slot only when its one probe resend is due, so other clients keep their sends ([MESSAGES.md m1.8](../standard/MESSAGES.md#m1.8), [m9.3](../standard/MESSAGES.md#m9.3)).
- A connection that sends a reliable message less than once a second again follows a rise in round-trip time: only a second in which an acknowledgement was owed counts as a silence for the round-trip filter, so its resend delay no longer stays low and resends every message ([MESSAGES.md m9.2](../standard/MESSAGES.md#m9.2)).
- `rocketnet_client_conditions` and `rocketnet_server_client_conditions` now write zeroes past the fields this library knows when `conditions_bytes` is larger than its `struct rocketnet_conditions_t`, so a program built against a later header reads zero in fields this version does not fill.
- The header and ERRORS-AND-LOGGING.md now say that `reliable_in_flight` counts every piece of a fragmented message from the moment its first piece is planned for sending, sent or not.
- USAGE.md now says that the outage guarantee covers a connected client, not the handshake: an outage of several seconds while connecting fails that attempt (`CONNECTION_RESPONSE_TIMED_OUT`), and the application connects again.
- ERRORS-AND-LOGGING.md's example policy now keys its saved refusal count by `rocketnet_server_client_connection_id`, so a slot reused inside one server update starts from zero.
- The standard (MESSAGES.md, TRANSPORT.md, RATIONALE.md) and the TLA+ models now state the v1.1.0 rules in full: refusal of a send with no room, the optional reliable timeout, probe pacing, the round-trip filter, the connect clock and the per-client scope.
- The macOS leak-check tests no longer hang on a forked child that the leak checker stops as it exits; test-only, the library is unchanged.

## Corrections to the v1.1.0 notes

- v1.1.0 also refuses connect tokens whose zero padding is not zero: the client refuses such a token as `INVALID_CONNECT_TOKEN`, the server drops a request whose signed padding is not zero (counted `CONNECTION_REQUEST_WRONG_VERSION`), and a sealed block whose padding is not zero after it opens is refused (counted `COULD_NOT_OPEN_SEALED_BLOCK` and charged to its source's failed-signature budget). Every v1.0.0 token writer writes zeros; a token writer of your own must too ([TRANSPORT.md t6](../standard/TRANSPORT.md#t6)).
- In v1.1.0 `rocketnet_client_required_memory` for the default client also grows, by 872 bytes (21,999,626 on macOS arm64); the notes gave only the light client's 296. Query it instead of reusing a v1.0.0 figure ([MEMORY.md](MEMORY.md)).
