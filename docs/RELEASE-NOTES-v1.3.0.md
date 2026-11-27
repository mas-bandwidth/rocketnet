# RocketNet v1.3.0

Changes compared with the `v1.2.2` tag. v1.2.3 was labelled on `main` but
never tagged or published, so its changes ship for the first time here.

## Wire change

A minor release with a new protocol version, `0x065014D9`: the connect token
and the connection request change size, the connection response carries a
packet size, and packet type 9 is the new connection probe
([TRANSPORT.md t6 Connect token](../standard/TRANSPORT.md#t6),
[t8.1 Connection request](../standard/TRANSPORT.md#t8.1),
[t8.4 Connection response](../standard/TRANSPORT.md#t8.4),
[t8.13 Connection probe](../standard/TRANSPORT.md#t8.13),
[t11.4 Path probe](../standard/TRANSPORT.md#t11.4)).

Compatibility: v1.3.0 endpoints do not interoperate with v1.2.x or earlier
endpoints, in either direction, and a v1.2.x connect token does not open on
v1.3.0. Clients, game servers and backends must be upgraded together.

The ABI changes too, so everything that includes `rocketnet.h` must be
rebuilt against v1.3.0: `rocketnet_connect_token_config_t` shrinks from 408
to 216 bytes, its `user_data` from 256 to 64 bytes, and `expire_seconds`,
`timeout_seconds` and `unix_timestamp` move from offsets 392, 396 and 400 to
200, 204 and 208; `ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES` goes from 256 to
64 and `ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES` from 1025 to 769; and
`rocketnet_client_config_t` grows from 128 to 168 bytes and
`rocketnet_server_config_t` from 256 to 288, with `link_mtus` and
`num_link_mtus` at the end. The rest of the public API only grows, and the
counter contract grows from 292 to 315 counters, appended at the end.

## Link MTU fallback

- Link MTU fallback. The client and server configs gain `link_mtus` and
  `num_link_mtus` (`ROCKETNET_CLIENT_LINK_MTUS`, `ROCKETNET_SERVER_LINK_MTUS`,
  a comma list): the IP MTUs of the links a path may cross, largest first, at
  most `ROCKETNET_MAX_LINK_MTUS` (8). The default, the one entry 1500, is
  exactly the previous behaviour: no probe, the same bytes on the wire. A
  longer list, such as the three-step list `1492,1200,576`, connects small
  and probes upward: the connection starts at the smallest packet size the
  list gives (the MTU less 28 over IPv4, less 48 over IPv6, IPv6 skipping
  entries below 1280), so every list joins exactly as fast as the default,
  and once connected the client probes the larger sizes one step at a time,
  smallest first, raising the connection's packet size to each one the path
  carries both ways. Before, a path that carried the handshake but not a full
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
  falls, and probing stops for good once it is the largest size. A size
  ruled out is tried again later, 10, 20 and 30 s after the rule-out, then
  at gaps doubling to 300 s and every 300 s after, so a size heavy loss
  ruled out is taken once the loss ends; a re-try that gets through raises
  the size and the walk goes on up, and the next size's schedule, should it
  be ruled out, starts afresh at 10 s. The sizes above a ruled-out size
  are never tried before the connection is at it. On a clean path the
  three-step list is at full size two round trips after connect, and on a
  1200 link at 1172 in one round trip, probing waiting in about half a
  second; on a 576 path 1452 is never sent. A path that does not carry even the smallest
  size ends the connection in the new client state
  `ROCKETNET_CLIENT_STATE_MTU_PROBE_FAILED` (-14).
  [TRANSPORT.md t2.3](../standard/TRANSPORT.md#t2.3),
  [t8.4](../standard/TRANSPORT.md#t8.4),
  [t8.13](../standard/TRANSPORT.md#t8.13),
  [t11.4](../standard/TRANSPORT.md#t11.4) and
  [t12](../standard/TRANSPORT.md#t12) rules 15 and 16.
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

## Client address on the server

- Public API: `rocketnet_server_client_address( server, client_index,
  address, address_bytes )` writes the address and port the connected client
  sends from, as the server sees them, as "a.b.c.d:port" or
  "[v6]:port". With a buffer of `ROCKETNET_MAX_ADDRESS_STRING_BYTES` it returns
  true exactly when `rocketnet_server_client_connected` does; a free slot, a
  stopped server, a bad index, a NULL server or a buffer too small for the
  address returns false and writes an empty string, as
  `rocketnet_server_ipv4_address` does (a NULL buffer writes nothing). The
  address is for diagnostics, support and logging; to look a client up, use
  its index or `rocketnet_server_client_id`. Do not ban by it: ban the
  account's user hash (`rocketnet_server_client_id`) and enforce the ban with
  per-server whitelists, as [Bans and whitelists](BANS-AND-WHITELISTS.md)
  describes.

## Examples

- Authentication and bans: the example backends sign players in through one
  seam, derive `client_id` as a keyed hash of the provider and user id, serve
  bans and a per-server whitelist, and give each user one seat; the example
  server fetches its whitelist and kicks a client the list does not hold
  ([Bans and whitelists](BANS-AND-WHITELISTS.md)). The match request is now
  `POST /match` with the sign-in ticket.
- The example server's whitelist fetch has one deadline for the whole
  request (connect, send and receive), so a backend that answers slowly or
  trickles its answer fails that fetch, and the server keeps its last good
  list, instead of holding the fetch thread and shutdown.
- The Go and C# example backends refuse a sign-in ticket holding any
  character outside the standard base64 alphabet and `=`, so both give the
  same answer to a ticket with whitespace in it.
- `examples/backend_server.c` prints each client's address when it connects.
- `example_backend_client` and `example_backend_server` take `--link-mtus`
  and print the connection's packet size, its largest unreliable message and
  its link MTU at connect and each time the size rises.

## Upgrade

Replace v1.2.2 with v1.3.0 everywhere at once: the client, the game server
and the backend. A v1.2.x endpoint cannot connect to a v1.3.0 one.

- Backends: issue the new connect token (576 bytes, a 768-character string)
  with at most 64 bytes of user data. Data that does not fit moves to the
  backend's own records, and the token carries a key to it. A backend built
  on the example also moves its match request to `POST /match`.
- Rebuild everything that includes `rocketnet.h` against the v1.3.0 header:
  a binary built against v1.2.x headers reads the connect token config and
  the client and server configs at the wrong offsets. Code that writes more
  than 64 bytes of user data, or relies on the old values of
  `ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES` or
  `ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES`, needs a source change.
- Games: otherwise no source change is needed. The default `link_mtus` list, `{ 1500 }`,
  behaves as v1.2.2 did. To use link MTU fallback, set the same list on the
  client and the server, and raise `reliable_messages_in_flight` if create
  returns `ROCKETNET_STATUS_LINK_MTUS_TOO_SMALL_FOR_MESSAGES`.
- Code that reads the packet size should read it per connection with the new
  calls, since a connection's size can start below 1452 and rise.
