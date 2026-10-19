# Big messages

A 200 KB reliable message, fragmented by the library and reassembled on the far
side, plus an unreliable send larger than one packet that the library promotes
to its reliable path. It is a server and a client in one process over the
in-memory `loopback` transport, so it needs no socket and no network.

## What it shows

1. Set `max_message_bytes` and `receive_ring_bytes` on both ends to 256 KB.
   The defaults (262144) already hold 200 KB; the example sets them explicitly.
2. Connect a client to a server with the published development keys and a
   development connect token.
3. Send a 200 KB `ROCKETNET_RELIABLE` message with `rocketnet_client_start_message`, a
   `memcpy` of a known pattern into the returned buffer, and
   `rocketnet_client_finish_message`. The server receives one reassembled message,
   checks every byte, and prints its size and the time it took to arrive.
4. Send 4096 bytes as `ROCKETNET_UNRELIABLE_WITH_ACK_RECEIPT`. That is above
   one packet's record limit, so the library promotes the send to its reliable
   path: the start message returns a buffer, and the finish message queues the
   promoted send. Every unreliable type is promoted the same way, up to
   `max_message_bytes`.
5. Read `ROCKETNET_CLIENT_COUNTER_RECEIPTS_ACKED` before and after the promoted
   send. It completes reliably with exactly one ACKED receipt, so it rises by one.

The public counters have no separate promotion counter, so this example reads
the one acknowledged receipt the promotion produces, `RECEIPTS_ACKED`.

Each frame runs update, receive and release, simulate, queue, then send last.
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

```
server received the 204800-byte reliable message intact after 1.234 s
promotion counter before: 0
promotion counter after: 1
PASS
```

The first line says the 200 KB message arrived whole and byte-exact. The next
two lines are the public receipt counter around the promoted send: one ACKED
receipt means it travelled the reliable path and was acknowledged. `PASS` means
the message was intact and the counter rose by one. The library's informational
log lines before it are not part of the check.

## Public calls it uses

- `rocketnet_init`, `rocketnet_term`
- `rocketnet_server_dev_config`, `rocketnet_server_create`, `rocketnet_server_start`, `rocketnet_server_update`,
  `rocketnet_server_port`, `rocketnet_server_max_clients`, `rocketnet_server_client_connected`,
  `rocketnet_server_receive_message`, `rocketnet_server_release_message`, `rocketnet_server_send_packets`,
  `rocketnet_server_destroy`
- `rocketnet_client_default_config`, `rocketnet_client_create`, `rocketnet_client_connect`, `rocketnet_client_state`,
  `rocketnet_client_update`, `rocketnet_client_start_message`, `rocketnet_client_finish_message`,
  `rocketnet_client_receive_message`, `rocketnet_client_release_message`, `rocketnet_client_send_packets`,
  `rocketnet_client_counter`, `rocketnet_client_disconnect`, `rocketnet_client_destroy`
- `rocketnet_write_dev_connect_token` (development library only)
- `rocketnet_time`, `rocketnet_sleep`
