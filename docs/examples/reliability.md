# Reliability

RocketNet offers five reliability families (plus three `*_WITH_ACK_RECEIPT`
variants) so each message pays only for the delivery and ordering guarantee it
needs.

| Mode | Delivered | Order | When to use it |
|---|---|---|---|
| `UNRELIABLE` (`_WITH_ACK_RECEIPT`) | At most once (may drop) | Arrival order | Ephemeral effects or delta-encoded state that never retransmits |
| `UNRELIABLE_SEQUENCED` | At most once (latest only) | Strictly increasing (drops older) | High-rate position or aim updates where stale packets are useless |
| `RELIABLE` (`_WITH_ACK_RECEIPT`) | Every message, once | Arrival order (no head-of-line wait) | Independent events that must arrive but do not depend on each other |
| `RELIABLE_ORDERED` (`_WITH_ACK_RECEIPT`) | Every message, once | Exact send order per channel | Chat, RPCs, inventory, and state transitions that require strict order |
| `RELIABLE_SEQUENCED` | Latest message guaranteed | Strictly increasing (supersedes older) | Infrequent state updates where only the newest state matters |

## What it shows

1. Start a server and a client over the in-memory `loopback` transport. Both
   ends run the development network simulator with 20 ms latency and 35 ms
   jitter. The server deliberately drops the first numbered send of each mode
   with 100 percent loss, then returns to zero loss for later sends and retries.
2. Send a numbered run of eight messages (`1..8`) in each of the eight
   reliability modes (one channel per mode), one message per frame. This makes
   loss certain: a random loss percentage need not drop any of eight messages.
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

One run is shown below; jitter and transport scheduling can change arrival order
and how many older sequenced messages are superseded. The deliberate first
unreliable send is always lost.

```
unreliable               sent=8 arrived=7 order=[3,4,2,5,6,8,7] gaps=1 duplicates=0 receipts=0/0
unreliable_sequenced     sent=8 arrived=6 order=[2,4,5,6,7,8] gaps=2 duplicates=0 receipts=0/0
reliable                 sent=8 arrived=8 order=[4,2,3,5,6,8,7,1] gaps=0 duplicates=0 receipts=0/0
reliable_ordered         sent=8 arrived=8 order=[1,2,3,4,5,6,7,8] gaps=0 duplicates=0 receipts=0/0
reliable_sequenced       sent=8 arrived=4 order=[3,4,5,8] gaps=4 duplicates=0 receipts=0/0
unreliable_receipt       sent=8 arrived=7 order=[2,3,5,4,6,7,8] gaps=1 duplicates=0 receipts=7/1
reliable_receipt         sent=8 arrived=8 order=[2,3,4,5,6,7,8,1] gaps=0 duplicates=0 receipts=8/0
reliable_ordered_receipt sent=8 arrived=8 order=[1,2,3,4,5,6,7,8] gaps=0 duplicates=0 receipts=8/0
PASS
```

The unsequenced unreliable modes deliver exactly `2..8` in arrival order;
`unreliable_receipt` reports `7` acked / `1` lost receipts. Sequenced modes never
step backwards, and the example checks that `reliable_sequenced` reaches the
final message `8`.
`reliable` and `reliable_receipt` deliver all `8` messages without head-of-line
stalls, while `reliable_ordered` and `reliable_ordered_receipt` deliver `1..8`
in exact sequence.

## Public calls it uses

- `rocketnet_init`, `rocketnet_term`
- `rocketnet_server_default_config`, `rocketnet_server_create`,
  `rocketnet_server_start`, `rocketnet_server_destroy`,
  `rocketnet_server_update`, `rocketnet_server_send_packets`,
  `rocketnet_server_ipv4_address`,
  `rocketnet_server_public_key`, `rocketnet_server_client_connected`,
  `rocketnet_server_can_send_message`, `rocketnet_server_start_message`, `rocketnet_server_finish_message`,
  `rocketnet_server_receive_messages`, `rocketnet_server_receipts`
- `rocketnet_client_default_config`, `rocketnet_client_create`,
  `rocketnet_client_connect`, `rocketnet_client_destroy`,
  `rocketnet_client_update`, `rocketnet_client_send_packets`,
  `rocketnet_client_state`, `rocketnet_client_index`,
  `rocketnet_client_can_send_message`, `rocketnet_client_start_message`, `rocketnet_client_finish_message`,
  `rocketnet_client_receive_messages`
- `rocketnet_client_network_simulator`, `rocketnet_server_network_simulator`
- `rocketnet_create_dev_connect_token` and
  `ROCKETNET_DEV_BACKEND_PUBLIC_KEY` (development library only)
- `rocketnet_status_name`, `rocketnet_time`, `rocketnet_sleep`
