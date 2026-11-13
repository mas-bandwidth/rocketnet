# RocketNet v1.1.50

Changes compared with the `v1.1.49` tag. A point release: no packet layout or
protocol version change, and v1.1.50 endpoints interoperate with every v1.x
endpoint.

- macOS server transport: rebuilt. Datagrams queued at a connected client
  socket that the server transport retired, and never handed to the game,
  are counted when the socket is closed and reported in a public counter.
- Counters: one new row, appended. 289 `RECEIVE_DROPPED_RETIRED`; the
  contract now holds 290 counters and no released id moved.

- Tag: `v1.1.50`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.50).
