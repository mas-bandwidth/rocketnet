# RocketNet v1.1.37

Changes compared with the `v1.1.36` tag. A point release: no configuration,
export, counter, packet layout or protocol version change, and v1.1.37
endpoints interoperate with every v1.x endpoint.

- Examples: the server examples (`client_server_server`, `backend_server` and
  `game_loop`) run in the shape of a game server's frame, with game threads
  writing each client's messages and the main thread updating the server in
  the three-call form. A small thread helper, `examples/thread.h`, is shared
  by the examples. The library is unchanged.

- Tag: `v1.1.37`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.37).
