# RocketNet v1.1.35

Changes compared with the `v1.1.34` tag. A point release: no configuration,
packet layout or protocol version change, and v1.1.35 endpoints interoperate
with every v1.x endpoint.

- Sending from game threads: `rocketnet_server_finish_client_message` now
  builds and sends an unreliable message small enough for one packet from the
  calling thread, instead of leaving it for `rocketnet_server_send_packets`.
  If the update's send limits leave no room, the call returns 0 and drops the
  message. Every other message is queued as before.
- Counters: one new counter, `MESSAGE_UNRELIABLE_WRITER_NO_CAPACITY` (id 281),
  counts those drops. Ids 0-280 are unchanged.

- Tag: `v1.1.35`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.35).
