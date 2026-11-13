# RocketNet v1.1.32

Changes compared with the `v1.1.31` tag. A point release: no configuration,
export, counter, packet layout or protocol version change, and v1.1.32
endpoints interoperate with every v1.x endpoint.

- Load test: the server load test tool (`harden_load`) gains the space-game
  frame shape: game threads that write each client's packets, a job pool, a
  game-load option and a row script, with the vendored enkiTS job system used
  by the test tool only. Test tooling and documentation only; the library is
  unchanged.

- Tag: `v1.1.32`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.32).
