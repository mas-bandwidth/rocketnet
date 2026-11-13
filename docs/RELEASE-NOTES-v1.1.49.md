# RocketNet v1.1.49

Changes compared with the `v1.1.48` tag. A point release: no packet layout or
protocol version change, and v1.1.49 endpoints interoperate with every v1.x
endpoint.

- Linux io_uring server transport: each pin an io_uring thread asks for and
  the system refuses is counted, and the datagrams the kernel dropped at its
  sockets are reported, each in a public counter.
- Counters: two new rows, appended. 287 `URING_PIN_REFUSALS` and 288
  `URING_RECEIVE_DROPPED_SOCKET`; the contract now holds 289 counters and no
  released id moved.

- Correction: `v1.1.49` was never tagged or released. The label was carried
  on `main`, and these changes shipped in
  [v1.1.50](RELEASE-NOTES-v1.1.50.md).
