# RocketNet v1.1.42

Changes compared with the `v1.1.41` tag. A point release: no configuration,
export, counter, packet layout or protocol version change, and v1.1.42
endpoints interoperate with every v1.x endpoint.

- Verification: the TLA+ model of the game-thread send rule and its trace
  check cover the point where a server first opts in to sending from game
  threads. Models, tests and comments only; behaviour is unchanged.

- Tag: `v1.1.42`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.42).
