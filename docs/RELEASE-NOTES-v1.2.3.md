# RocketNet v1.2.3

Changes compared with the `v1.2.2` tag. A point release: no packet layout or
protocol version change and no new counter (the contract still holds 292
counters), and v1.2.3 endpoints interoperate with every v1.x endpoint. The
public API gains one call; the ABI only grows.

- Public API: `rocketnet_server_client_address( server, client_index,
  address, address_bytes )` writes the address and port a connected client's
  packets come from, as the server sees them, as "a.b.c.d:port" or
  "[v6]:port". With a buffer of `ROCKETNET_MAX_ADDRESS_STRING_BYTES` it returns
  true exactly when `rocketnet_server_client_connected` does; a free slot, a
  stopped server, a bad index, a NULL server or a buffer too small for the
  address returns false and writes an empty string, as
  `rocketnet_server_ipv4_address` does (a NULL buffer writes nothing). The address is for diagnostics, support
  and logging; to look a client up, use its index or
  `rocketnet_server_client_id`. Do not ban by it: ban the account's user
  hash (`rocketnet_server_client_id`) and enforce the ban with per-server
  whitelists, as [Bans and whitelists](BANS-AND-WHITELISTS.md) describes.
- Backend example: `examples/backend_server.c` prints each client's address
  when it connects.
- Authentication and bans: the example backends sign players in through one
  seam, derive `client_id` as a keyed hash of the provider and user id, serve
  bans and a per-server whitelist, and give each user one seat; the example
  server fetches its whitelist and kicks a client the list does not hold
  ([Bans and whitelists](BANS-AND-WHITELISTS.md)). The match request is now
  `POST /match` with the sign-in ticket.

Upgrade: replace v1.2.2 with v1.2.3. A game that calls the library needs no
source change; a backend built on the example moves its match request to
`POST /match`.
