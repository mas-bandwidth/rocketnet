# Ack Receipts

A game sends state updates unreliably because a lost update is superseded by the next frame's update, never retransmitted.
Delta compression still needs to know the newest update the client actually received so the next frame can encode against that baseline.
Use `ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT` to receive ACKED or LOSS
outcomes for messages that fit one packet. Larger sends promote to reliable
delivery. The receipt ring is bounded, and sends still in flight when a
connection ends get no terminal outcome; see [receipts](../CHANNELS.md#receipts-and-stats).

## What it shows

1. Create a server and a client over the `loopback` transport, with 30 percent loss on outgoing client packets from `rocketnet_client_network_simulator`, so some packets arrive and some are lost.
2. Queue 20 state updates, and more (at most 200) until both outcomes have been seen, with `ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT` (`rocketnet_client_can_send_message`, `rocketnet_client_start_message` and `rocketnet_client_finish_message`), recording the nonzero serial returned for each update.
3. Read `rocketnet_client_receipts` each frame after `rocketnet_client_update` and match each `rocketnet_receipt_t` serial back to the update that sent it.
4. Print `ACKED` (`ROCKETNET_RECEIPT_STATUS_ACKED`) or `LOSS` (`ROCKETNET_RECEIPT_STATUS_LOSS`) for each update and verify that every send receives exactly one receipt.

Each frame runs the five steps in game-loop order: update, drain the receive queue (plus receipts), simulate, queue outgoing messages, and `send_packets` last.

## Run it

From the repository root, after the normal build:

```sh
./bin/example_ack_receipts
```

On Windows the binary is `bin\Release\example_ack_receipts.exe`. The program finishes within a few seconds and prints `PASS` as its last line; any failure prints one `FAIL: ...` line and exits 1.

## What the output means

```
update 0 (serial 1): ACKED
...
update 5 (serial 6): LOSS
...
receipts resolved: 20/20 (acked 14, loss 6)
PASS
```

Each `update <n> (serial <s>): ACKED|LOSS` line reports the single outcome for that send. A game can use its newest acknowledged update as a delta baseline, provided
the receiver retains that state. ACKED is a transport acknowledgement, not
an application commit, and LOSS can also mean a missing return acknowledgement.
Lost updates in this example are skipped without retransmission. `PASS` means every send received one receipt with both `ACKED` and `LOSS` outcomes observed.

<a id="public-calls-it-uses"></a>

Source: [`examples/ack_receipts.c`](../../examples/ack_receipts.c). See the
[integration guide](../USAGE.md) for shared lifecycle, threading, and message
ownership rules.
