# Ack Receipts

A game sends state updates unreliably because a lost update is superseded by the next frame's update, never retransmitted.
Delta compression still needs to know the newest update the client actually received so the next frame can encode against that baseline.
Sending with `ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT` gives each update one receipt (`ROCKETNET_RECEIPT_STATUS_ACKED` or `ROCKETNET_RECEIPT_STATUS_LOSS`) without retransmitting lost ones.

## What it shows

1. Create a server and a client over the `loopback` transport, with 30 percent loss on outgoing client packets from `rocketnet_client_network_simulator`, so some packets arrive and some are lost.
2. Queue 20 state updates with `ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT` (`rocketnet_client_start_message` and `rocketnet_client_finish_message`), recording the nonzero serial returned for each update.
3. Read `rocketnet_client_receipts` each frame after `rocketnet_client_update` and match each `rocketnet_receipt_t` serial back to the update that sent it.
4. Print `ACKED` (`ROCKETNET_RECEIPT_STATUS_ACKED`) or `LOSS` (`ROCKETNET_RECEIPT_STATUS_LOSS`) for each update and verify that all 20 sends receive exactly one receipt.

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

Each `update <n> (serial <s>): ACKED|LOSS` line reports the single outcome for that send. Acked updates advance a delta-compression baseline; lost updates are skipped without retransmission. `PASS` means every send received one receipt with both `ACKED` and `LOSS` outcomes observed.

## Public calls it uses

- `rocketnet_init`, `rocketnet_term`
- `rocketnet_server_default_config`, `rocketnet_server_create`,
  `rocketnet_server_start`, `rocketnet_server_destroy`,
  `rocketnet_server_update`, `rocketnet_server_send_packets`,
  `rocketnet_server_ipv4_address`,
  `rocketnet_server_public_key`, `rocketnet_server_client_connected`,
  `rocketnet_server_start_message`, `rocketnet_server_finish_message`,
  `rocketnet_server_receive_messages`
- `rocketnet_client_default_config`, `rocketnet_client_create`,
  `rocketnet_client_connect`, `rocketnet_client_destroy`,
  `rocketnet_client_update`, `rocketnet_client_send_packets`,
  `rocketnet_client_state`, `rocketnet_client_index`,
  `rocketnet_client_start_message`, `rocketnet_client_finish_message`,
  `rocketnet_client_receive_messages`, `rocketnet_client_receipts`,
  `rocketnet_client_network_simulator`
- `rocketnet_create_dev_connect_token` and
  `ROCKETNET_DEV_BACKEND_PUBLIC_KEY` (development library only)
- `rocketnet_status_name`, `rocketnet_time`, `rocketnet_sleep`
