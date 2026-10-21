# Big messages

A 200 KB reliable message, fragmented by the library and reassembled on the far
side, plus an unreliable send larger than one packet that the library promotes
to its reliable path. It is a server and a client in one process over the
in-memory `loopback` transport, so it needs no socket and no network.

## What it shows

1. Set `max_message_bytes` and `receive_message_buffer_bytes` on both ends to 256 KB.
   The defaults (262144) already hold 200 KB; the example sets them explicitly.
2. Configure the development backend public key with
   `ROCKETNET_DEV_BACKEND_PUBLIC_KEY`; server create generates the server key.
   Read its public key and bound address after start, then create an opaque
   base64 development token with `rocketnet_create_dev_connect_token`.
3. Send a 200 KB `ROCKETNET_RELIABILITY_RELIABLE` message with `rocketnet_client_start_message`, a
   known pattern written byte by byte straight into the returned buffer, and
   `rocketnet_client_finish_message`. The server receives one reassembled message,
   checks every byte, and prints its size and the time it took to arrive.
4. Send 4096 bytes as `ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT`. That is above
   one packet's record limit, so the library promotes the send to its reliable
   path. Plain `ROCKETNET_RELIABILITY_UNRELIABLE` is promoted the same way;
   the receipt variant additionally reports the acknowledgement.
5. Call `rocketnet_client_stats` before and after the promoted receipt send and
   compare `rocketnet_client_stats_t.receipts_acked`. The send completes
   reliably with exactly one ACKED receipt, so the field rises by one.

The public stats expose the acknowledged receipt the promotion
produces. Oversize unreliable messages use the reliable path rather than being
refused; selecting the receipt mode determines whether the sender receives an
outcome.

Each frame runs update, drains the receive queue, simulates, queues, then sends last.
Sending last is what makes this frame's messages leave this frame.

## Run it

From the repository root, after the normal build:

```sh
./bin/example_big_messages
```

On Windows the binary is `bin\Release\example_big_messages.exe`. The program
finishes well inside twenty seconds and prints `PASS` as its last line; any
failure prints one `FAIL: ...` line and exits 1.

## What the output means

The program prints the byte count and measured arrival time for the reassembled
message, then the `receipts_acked` value before and after the promoted send.
The exact time is run-dependent. A successful check requires the 200 KB body
to be byte-exact and the after value to equal the before value plus one. The
library's informational log lines are not part of the check.

## Public calls it uses

- `rocketnet_init`, `rocketnet_term`
- `rocketnet_server_default_config`, `rocketnet_server_create`,
  `rocketnet_server_start`, `rocketnet_server_destroy`,
  `rocketnet_server_update`, `rocketnet_server_send_packets`,
  `rocketnet_server_port`, `rocketnet_server_ipv4_address`,
  `rocketnet_server_public_key`, `rocketnet_server_receive_messages`
- `rocketnet_client_default_config`, `rocketnet_client_create`,
  `rocketnet_client_connect`, `rocketnet_client_destroy`,
  `rocketnet_client_update`, `rocketnet_client_send_packets`,
  `rocketnet_client_disconnect`, `rocketnet_client_state`,
  `rocketnet_client_start_message`, `rocketnet_client_finish_message`,
  `rocketnet_client_receive_messages`, `rocketnet_client_stats`
- `rocketnet_create_dev_connect_token` and
  `ROCKETNET_DEV_BACKEND_PUBLIC_KEY` (development library only)
- `rocketnet_status_name`, `rocketnet_time`, `rocketnet_sleep`
