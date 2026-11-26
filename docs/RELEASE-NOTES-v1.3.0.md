# RocketNet v1.3.0

Changes compared with the `v1.2.2` tag. A minor release with a new protocol
version: the connect token and the connection request change size, the
connection response carries a packet size, and packet type 9 is the new
connection probe, so v1.3.0 endpoints do not interoperate with v1.2.x
endpoints. Backends must issue the new token format. The counter contract
grows from 292 to 315 counters, appended at the end.

- Link MTU fallback. The client and server configs gain `link_mtus` and
  `num_link_mtus` (`ROCKETNET_CLIENT_LINK_MTUS`, `ROCKETNET_SERVER_LINK_MTUS`,
  a comma list): the IP MTUs of the links a path may cross, largest first, at
  most `ROCKETNET_MAX_LINK_MTUS` (8). The default, the one entry 1500, is
  exactly the previous behaviour: no probe, the same bytes on the wire. A
  longer list, such as the three-step list `1492,1200,576`, connects small
  and probes upward: the connection starts at the smallest packet size the
  list gives (the MTU less 28 over IPv4, less 48 over IPv6, IPv6 skipping
  entries below 1280), so every list joins exactly as fast as the default,
  and once connected the client probes the larger sizes, largest first, and
  raises the connection's packet size to the largest one the path carries
  both ways. Before, a path that carried the handshake but not a full
  1452-byte packet connected and then never delivered a message that needed
  a full packet, while keep-alives held the connection open; now the
  connection only ever sends at a size the path has carried.
  Each probe is a pair, a small probe and a large one, sealed under the
  session keys, so only the two ends of the connection can make one; a forged
  probe spends nothing. A large probe that goes unanswered while its small
  partner comes back is a strike, and four strikes in a row rule a size out
  at the probe's own pace, ten pairs a second; a pair whose small probe is
  lost too sets the strikes back to zero, so loss alone, which takes the
  small probe as well, never does. The client raises on the server's authenticated reply and
  confirms; the server raises only on the confirm. The packet size never
  falls, and probing stops for good once it is the largest size or every
  larger size is ruled out. On a clean path the connection is at full size
  one round trip after connect, and on a 1200 link at 1172 in about half a
  second. A path that does not carry even the smallest
  size ends the connection in the new client state
  `ROCKETNET_CLIENT_STATE_MTU_PROBE_FAILED` (-14).
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
  transport: the ack block, the payload capacity and the unreliable
  one-record limit follow it, and the block stride is fixed by the size the
  connection starts at (standard/MESSAGES.md m2.2). New calls report the
  current value, which only grows: `rocketnet_client_max_packet_bytes`,
  `rocketnet_client_max_unreliable_message_bytes`,
  `rocketnet_server_client_max_packet_bytes` and
  `rocketnet_server_client_max_unreliable_message_bytes`; and
  `rocketnet_client_link_mtu` and `rocketnet_server_client_link_mtu` report
  the entry of the `link_mtus` list that size comes from (the first, largest,
  where entries give the same size, so 1500 for `{ 1500, 1492 }` over IPv4),
  0 while not connected.
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
  `CLIENT_PROBE_STRIKE`, `CLIENT_PROBE_RULED_OUT`, `CLIENT_PROBE_FAILED`,
  `CONNECTION_PROBE_CONFIRMED` and `CONNECTION_RESPONSE_SIZE_NOT_ALLOWED`.
  The kernel filter accepts type 9 from 34 bytes to the receive ceiling.
