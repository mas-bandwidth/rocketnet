# RocketNet v1.1.25

Changes compared with the `v1.1.24` tag. A point release: no configuration,
export, counter, packet layout or protocol version change, and v1.1.25
endpoints interoperate with every v1.x endpoint.

- Receive prefilter: the shared receive prefilter header gains the worker
  form, which a transport's receive worker uses to drop hostile packets before
  they reach the main thread, with its unit test.

- Tag: `v1.1.25`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.25).
