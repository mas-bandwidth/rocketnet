# RocketNet v1.1.44

Changes compared with the `v1.1.43` tag. A point release: no configuration,
export, counter, packet layout or protocol version change, and v1.1.44
endpoints interoperate with every v1.x endpoint.

- XDP server transport: it now tells the send reserve how many send queues
  it has, which queue a destination hashes to, and each queue's own free
  room, so the reserve no longer judges every queue by the fullest one. An
  index outside the queues has no room. A unit case covers it.

- Tag: `v1.1.44`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.44).
