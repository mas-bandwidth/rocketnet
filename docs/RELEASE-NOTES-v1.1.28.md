# RocketNet v1.1.28

Changes compared with the `v1.1.27` tag. A point release: no configuration,
counter, packet layout or protocol version change, and v1.1.28 endpoints
interoperate with every v1.x endpoint.

- Status: `ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED` is renamed
  `ROCKETNET_STATUS_NO_TRANSPORT_FOUND`. This is a source rename: the value
  (-18) is unchanged, so a built program keeps working, and a program that names
  the old constant must use the new name when it is recompiled. The comment on
  the status says what it means: every transport this server or client was
  allowed to try failed to start.

- Tag: `v1.1.28`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.28).
