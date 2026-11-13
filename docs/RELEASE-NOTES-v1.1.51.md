# RocketNet v1.1.51

Changes compared with the `v1.1.50` tag. A point release: no packet layout or
protocol version change, and v1.1.51 endpoints interoperate with every v1.x
endpoint.

- No API or counter change: the contract holds 290 counters and no released
  id moved.
- The word "ladder" is retired. The TLA+ model RNClientLadder is now
  RNClientTransportOrder, and the test, fuzz-corpus and tool names that
  carried the word are renamed with it.
- Released notes, the preview qualification records and the dated audits are
  left as written.

- Tag: `v1.1.51`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.51).
