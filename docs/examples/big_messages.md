# Big messages

A 200 KiB reliable message, fragmented by the library and reassembled on the far
side, plus an unreliable send larger than one packet that the library promotes
to its reliable path. It is a server and a client in one process over the
in-memory `loopback` transport, so it needs no socket and no network.

## What it shows

1. Set `max_message_bytes` and `receive_message_buffer_bytes` on both ends to 256 KiB.
   The 1 MiB defaults already hold 200 KiB; this example uses a smaller explicit profile.
2. Configure the development backend public key with
   `ROCKETNET_DEV_BACKEND_PUBLIC_KEY`; server create generates the server key.
   Read its public key and bound address after start, then create an opaque
   base64 development token with `rocketnet_create_dev_connect_token`.
3. Send a 200 KiB `ROCKETNET_RELIABILITY_RELIABLE` message with `rocketnet_client_can_send_message`, `rocketnet_client_start_message`, a
   known pattern copied into the returned buffer, and
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
Sending last makes queued work available to the transport without an avoidable
frame of delay; a background worker may transmit it later.

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
The exact time is run-dependent. A successful check requires the 200 KiB body
to be byte-exact and the after value to equal the before value plus one. The
library's informational log lines are not part of the check.

<a id="public-calls-it-uses"></a>

Source: [`examples/big_messages.c`](../../examples/big_messages.c). See the
[integration guide](../USAGE.md) for shared lifecycle, threading, and message
ownership rules.
