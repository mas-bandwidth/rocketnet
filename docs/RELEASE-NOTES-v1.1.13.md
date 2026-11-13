# RocketNet v1.1.13

Changes compared with the `v1.1.12` tag. A point release: no configuration,
export, counter, packet layout or protocol version change, and v1.1.13
endpoints interoperate with every v1.x endpoint.

- Client (Linux `gso` transport): a full socket no longer drops the rest of a
  send batch; the unsent datagrams stay queued and are sent on the next try.
- Server: a connection whose first keep-alive cannot be written counts
  `CONNECTION_RESPONSE_INTERNAL_ERROR` only, not also
  `CONNECTION_RESPONSE_CONNECTED`.
- Message layer: a corrupt resend mark is refused before it is followed, and
  the connection ends, instead of reading outside the resend queue.

- Tag: `v1.1.13`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.13).
