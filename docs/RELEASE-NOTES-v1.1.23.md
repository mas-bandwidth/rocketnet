# RocketNet v1.1.23

Changes compared with the `v1.1.22` tag. A point release: no counter, packet
layout or protocol version change, and v1.1.23 endpoints interoperate with
every v1.x endpoint.

- Server configuration: `bind_ipv4_address` and `bind_ipv6_address` choose the
  address the server binds per address family. They are new fields at the end of
  `rocketnet_server_config_t`, which grows from 200 to 216 bytes. Left NULL or
  empty, the server binds the wildcard address as before.
- Status: `ROCKETNET_STATUS_TRANSPORT_BIND_REFUSED` (-35). A bind the system
  refuses is reported by name when the server starts.

- Tag: `v1.1.23`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.23).
