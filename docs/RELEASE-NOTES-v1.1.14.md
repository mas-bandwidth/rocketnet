# RocketNet v1.1.14

Changes compared with the `v1.1.13` tag. A point release: no configuration,
export, counter, packet layout or protocol version change, and v1.1.14
endpoints interoperate with every v1.x endpoint.

- Tests only: the clean-run check no longer counts a withheld cookie reply
  copy as a refusal when a cookie reply went out (an update late by a few
  send intervals banks that many copies). This removes a load flake.

- Tag: `v1.1.14`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.14).
