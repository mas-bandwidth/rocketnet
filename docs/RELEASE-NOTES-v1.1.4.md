# RocketNet v1.1.4 (draft)

Changes compared with the `v1.1.3` tag. A point release: no configuration
field, export, status value, packet layout or protocol version changes, and
v1.0.0 through v1.1.4 endpoints interoperate in both directions.

## Fixes since v1.1.3

- When a connection's send budget is short, the sender writes its packet before it takes a frame. If the transport then refused the frame (an XDP server with no link-layer address for the client yet, a client whose send ring is full, or a full lease table), v1.1.3 dropped every unreliable message the packet had taken, counted each as a send failure, and went on to drop the rest of that connection's unreliable queue the same way. Now a refused frame leaves the unreliable queue as it was: nothing is sent, dropped or counted, and the messages go in a later update. This is the behaviour of v1.1.0 and earlier.
- `scripts/rocketnet-provision` check R8 no longer passes without checking on a 6.12 or later kernel when `/boot/config-*` is absent. It reads `/proc/config.gz` when present; when neither is readable and the throttle sysctl is not already off, `verify` fails with "could not verify R8" instead of passing. The script's selftest settings are now ignored outside selftest mode.
- The `connections_denied_full` counter's documentation now states that since v1.1.2 it also counts a connection response ignored because the only free slots were freed in the same update.
- New tests cover the refused frame on each path above, the server send cursor at a later-round capacity cut, and the per-destination segmentation refusal table with more refusing destinations than it holds; test changes only.

## Corrections to earlier notes

- **v1.1.2, required memory.** The v1.1.2 notes did not say that the per-destination segmentation refusal table grew the server's required memory. Measured against v1.1.1 (the planned server memory), a gso server needs 1,296 more bytes and an io_uring server 1,344 more, the same at 1 and at 1,000 clients; the portable transport is unchanged.
- **v1.1.3, refused frames.** The v1.1.3 notes did not say that a refused frame after a budget-short write dropped unreliable messages, described in the first fix above. It was introduced in v1.1.3 and is fixed in v1.1.4. Reliable messages were not affected: they stayed queued and were sent again.
