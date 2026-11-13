# RocketNet v1.1.38

Changes compared with the `v1.1.37` tag. A point release: no configuration,
export, counter, packet layout or protocol version change, and v1.1.38
endpoints interoperate with every v1.x endpoint.

- Linux XDP server transport: the receive pass, the replay sink and the send
  staging are rebuilt, each bound to its TLA+ model by a trace check. A network
  thread whose CPU pin is refused stays on the allowed CPUs less CPU 0. No
  public API change.

- Tag: `v1.1.38`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.38).
