# RocketNet v1.1.6

Changes compared with the `v1.1.4` tag. A point release: no configuration
field, export, status value, packet layout or protocol version changes, and
v1.0.0, v1.1.0, v1.1.1, v1.1.2, v1.1.3, v1.1.4 and v1.1.6 endpoints
interoperate in both directions.

- Tag: `v1.1.6`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.6).

v1.1.5 was not published; its changes are in this release ([v1.1.5 notes](RELEASE-NOTES-v1.1.5.md)).

## Since v1.1.5

v1.1.6 changes no library code: tests, tools, CI and documentation only.

- More unit cases cover the send window, acknowledgements, the message send path, unreliable and sequenced messages, hostile receive records sent by the other side of the connection, the held-message index and the session keys a connect token carries, each one shown to fail when the library code it covers is broken. The deterministic simulation checks that every failed send still reports its outcome to the sender, under a new `send-fail` schedule run nightly (DST.md).
- Test repairs: a hard-killed client's slot is checked to be freed no sooner than its timeout; the per-family byte-count and batch chaff cases run their exchange once more on fresh sockets after one lost loopback datagram, and still fail on a second loss or a double count; the macOS server create/destroy x1000 case runs as one case per transport; the happy-path tool bounds every wait even when the nightly tier lifts its deadline; the fuzz harnesses refuse a seed path too long for their buffer; a refused connect in the 1000-client world prints its status.
- Windows: the development library's Debug functional suite now runs to completion under the MSVC debug CRT. Run it with `ROCKETNET_TEST_CASE_TIME_SCALE=2`, or 4 in the nightly tier (WINDOWS.md). The soak's resource check waits for the operating system's thread pool to retire its workers, and the Application Verifier script removes its own registry key after a run (BUILD-REFERENCE.md).
- CI: the macOS happy-path job keeps Go's build and module caches on the runner's boot volume.
