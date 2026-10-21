# Reliability

RocketNet offers five reliability families (plus three `*_WITH_ACK_RECEIPT`
variants) so each message pays only for the delivery and ordering guarantee it
needs.

| Mode | Delivered | Order | When to use it |
|---|---|---|---|
| `UNRELIABLE` (`_WITH_ACK_RECEIPT`) | At most once (may drop) | Arrival order | Ephemeral effects or delta-baseline snapshots that never retransmit |
| `UNRELIABLE_SEQUENCED` | At most once (latest only) | Strictly increasing (drops older) | High-rate position or aim updates where stale packets are useless |
| `RELIABLE` (`_WITH_ACK_RECEIPT`) | Every message, once | Arrival order (no head-of-line wait) | Independent events that must arrive but do not depend on each other |
| `RELIABLE_ORDERED` (`_WITH_ACK_RECEIPT`) | Every message, once | Exact send order per channel | Chat, RPCs, inventory, and state transitions that require strict order |
| `RELIABLE_SEQUENCED` | Latest message guaranteed | Strictly increasing (supersedes older) | Infrequent state snapshots where only the newest state matters |

## What it shows

1. Start a server and a step-driven client over the in-memory `loopback`
   transport with `simulated_loss_percent = 25`, `simulated_latency_ms = 20`,
   and `simulated_jitter_ms = 35`.
2. Send a numbered run of eight messages (`1..8`) in each of the eight
   reliability modes (one channel per mode), one message per frame so each send
   faces independent simulated loss and jitter.
3. Receive messages across all channels, collect resolved receipts from
   `rocketnet_server_receipts`, print one summary line per mode, and check each
   mode's delivery, ordering, and receipt promise.

Each frame runs in standard order: `update`, drain the receive queue (plus
`receipts`), simulation, `start_message` and `finish_message`, and
`send_packets` last so queued messages leave in the same frame.

## Run it

From the repository root, after the normal build:

```sh
./bin/example_reliability
```

## What the output means

```
unreliable               sent=8 arrived=5 order=[1,3,6,7,8] gaps=3 duplicates=0 receipts=0/0
unreliable_sequenced     sent=8 arrived=5 order=[1,3,6,7,8] gaps=3 duplicates=0 receipts=0/0
reliable                 sent=8 arrived=8 order=[1,3,2,4,5,6,7,8] gaps=0 duplicates=0 receipts=0/0
reliable_ordered         sent=8 arrived=8 order=[1,2,3,4,5,6,7,8] gaps=0 duplicates=0 receipts=0/0
reliable_sequenced       sent=8 arrived=5 order=[1,2,4,7,8] gaps=3 duplicates=0 receipts=0/0
unreliable_receipt       sent=8 arrived=6 order=[3,2,4,5,7,8] gaps=2 duplicates=0 receipts=6/2
reliable_receipt         sent=8 arrived=8 order=[1,2,3,6,8,7,4,5] gaps=0 duplicates=0 receipts=8/0
reliable_ordered_receipt sent=8 arrived=8 order=[1,2,3,4,5,6,7,8] gaps=0 duplicates=0 receipts=8/0
PASS
```

Unreliable modes drop lost packets; `unreliable_receipt` shows jitter delivering
`3` before `2` and reports `6` acked / `2` lost receipts. Sequenced modes never
step backwards, and `reliable_sequenced` guarantees the final message `8`.
`reliable` and `reliable_receipt` deliver all `8` messages without head-of-line
stalls, while `reliable_ordered` and `reliable_ordered_receipt` deliver `1..8`
in exact sequence.

## Public calls it uses

- `rocketnet_init`, `rocketnet_term`
- `rocketnet_server_default_config`, `rocketnet_server_create`,
  `rocketnet_server_start`, `rocketnet_server_destroy`,
  `rocketnet_server_update`, `rocketnet_server_send_packets`,
  `rocketnet_server_port`, `rocketnet_server_ipv4_address`,
  `rocketnet_server_public_key`, `rocketnet_server_client_connected`,
  `rocketnet_server_start_message`, `rocketnet_server_finish_message`,
  `rocketnet_server_receive_messages`, `rocketnet_server_receipts`
- `rocketnet_client_default_config`, `rocketnet_client_create`,
  `rocketnet_client_connect`, `rocketnet_client_destroy`,
  `rocketnet_client_update`, `rocketnet_client_send_packets`,
  `rocketnet_client_state`, `rocketnet_client_index`,
  `rocketnet_client_start_message`, `rocketnet_client_finish_message`,
  `rocketnet_client_receive_messages`
- `rocketnet_create_dev_connect_token` and
  `ROCKETNET_DEV_BACKEND_PUBLIC_KEY` (development library only)
- `rocketnet_status_name`, `rocketnet_time`, `rocketnet_sleep`
