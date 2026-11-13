# RocketNet v1.1.36

Changes compared with the `v1.1.35` tag. A point release: no configuration,
counter, packet layout or protocol version change, and v1.1.36 endpoints
interoperate with every v1.x endpoint.

- Server update in three calls: `rocketnet_server_update_receive`,
  `rocketnet_server_update_client` and `rocketnet_server_update_finish` are
  `rocketnet_server_update` split up, for a game that wants its game threads to
  apply the acknowledgements received from each client. Call `update_receive`
  and `update_finish` on the main thread and `update_client` once for each
  client index on the game threads in between; the result is the same as
  `rocketnet_server_update`. A server updated by one `rocketnet_server_update`
  call needs nothing new. These are three new exports in `sdk/rocketnet.h`.

- Tag: `v1.1.36`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.36).
