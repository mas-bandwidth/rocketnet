# RocketNet v1.3.0

Changes compared with the `v1.2.2` tag. A minor release with a new protocol
version: the connect token and the connection request change size, the
connection response carries a packet size, and packet type 9 is the new
connection probe, so v1.3.0 endpoints do not interoperate with v1.2.x
endpoints. Backends must issue the new token format. The counter contract
grows from 292 to 313 counters, appended at the end.

- Link MTU fallback. The client and server configs gain `link_mtus` and
  `num_link_mtus` (`ROCKETNET_CLIENT_LINK_MTUS`, `ROCKETNET_SERVER_LINK_MTUS`,
  a comma list): the IP MTUs of the links a path may cross, largest first, at
  most `ROCKETNET_MAX_LINK_MTUS` (8). The default, the one entry 1500, is
  exactly the previous behaviour: no probe, the same bytes on the wire. A
  longer list, such as the three-step list `1492,1200,576`, makes the client
  probe the path after the challenge and connect at the largest packet size
  carried both ways (the MTU less 28 over IPv4, less 48 over IPv6, IPv6
  skipping entries below 1280). Before, a path that carried the handshake but
  not a full 1452-byte packet connected and then never delivered a message
  that needed a full packet, while keep-alives held the connection open; now
  the connection runs at a size the path carries, or the attempt ends in the
  new client state `ROCKETNET_CLIENT_STATE_MTU_PROBE_FAILED` (-14).
  Probes are sealed under the session keys, so only the two ends of the
  challenge can make one; a forged probe spends nothing. A window is at most
  10 sends, so a walk of the longest list ends within 8 s; a server whose
  list probes holds the half-open mapping 8 s longer, and the client's
  silence timeout waits while it probes.
  standard/TRANSPORT.md t2.3, t8.4, t8.13, t11.4 and t12 rules 15 and 16;
  proofs/tla/RNPathProbe.tla.
- Create refuses a bad list with its own status:
  `ROCKETNET_STATUS_LINK_MTUS_EMPTY` (-36), `_NOT_DESCENDING` (-37),
  `_TOO_LARGE` (-38), `_TOO_SMALL` (-39, an entry below 546), and
  `_TOO_SMALL_FOR_MESSAGES` (-40), when the smallest packet size cannot carry
  `max_message_bytes` within `reliable_messages_in_flight` blocks. With the
  default 1 MiB message and window of 1024, a list whose smallest entry is
  below 1109 needs a larger window: the three-step list needs 4096.
- Each connection has its own packet size, at both ends and on every
  transport: the ack block, the payload capacity, the block stride and the
  unreliable one-record limit follow it (standard/MESSAGES.md m2.2). New calls
  report it: `rocketnet_client_max_packet_bytes`,
  `rocketnet_client_max_unreliable_message_bytes`,
  `rocketnet_server_client_max_packet_bytes` and
  `rocketnet_server_client_max_unreliable_message_bytes`.
- Connect token user data goes from 256 to 64 bytes, so the connection
  request (518 bytes) fits a 576-byte IPv4 MTU. User data is meant for an
  index or key into the backend's own records, such as a session key or a
  user hash, not the data itself; the game server fetches what else it needs
  from the backend after connect. The token is 576 bytes and its string 768
  characters; the protocol version is `0x065014D9`. The Go and C# example
  backends write the new format.
- Counters: the connection probe family at the server
  (`CONNECTION_PROBE_RECEIVED` and its outcomes), the client's probe family
  (`CLIENT_PROBE_RECEIVED` and its outcomes), `CLIENT_PROBE_SENT`,
  `CLIENT_PROBE_WINDOW_EXPIRED`, `CLIENT_PROBE_FAILED` and
  `CONNECTION_RESPONSE_SIZE_NOT_ALLOWED`. The kernel filter accepts type 9
  from 518 bytes to the receive ceiling.
