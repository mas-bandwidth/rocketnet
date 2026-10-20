# Channels

Games put different traffic on different channels so a lost reliable message
(like chat or an inventory change) never stalls time-critical state updates
(like player positions) that only care about the newest value.

## What it shows

1. Configure a server and client over the in-memory `loopback` transport with
   `simulated_loss_percent = 35` on the client and a 120 ms retransmit timeout.
2. Send eight `ROCKETNET_RELIABILITY_RELIABLE_ORDERED` chat messages on channel 0 while
   sending `ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED` position updates every frame on
   channel 1.
3. Observe that when packet loss drops a chat message, later chat messages on
   channel 0 are held until the missing message is resent, while newer position
   updates on channel 1 keep arriving without waiting.
4. Verify that every chat message is delivered in exact order `1..8` and that
   position updates never go backwards.

Each frame follows the standard loop order: `update`, `receive` and `release`,
simulation, `start` and `finish`, and `send_packets` last so queued messages
leave in the same frame. The header at this base still declares `start`,
`finish`, `receive`, `release` and `send_packets` (renamed to `*_message` /
`*_messages` by the lifecycle card), so the example uses the header names.

## Run it

From the repository root, after the normal build:

```sh
./bin/example_channels
```

## What the output means

```
t=0.025s positions=3 chat=2/5 (chat held while positions arrive)
...
t=0.155s positions=20 chat=8/8 (chat delivered in order)
PASS
```

Lines marked `chat held while positions arrive` show `positions` advancing on
channel 1 while channel 0 waits for a lost chat message to be retransmitted.
Lines marked `chat delivered in order` show channel 0 releasing held chat
messages in sequence once the retransmission arrives. `PASS` confirms both
conditions held.

## Public calls it uses

- `rocketnet_init`, `rocketnet_term`
- `rocketnet_server_dev_config_sized`, `rocketnet_server_create_sized`,
  `rocketnet_server_port`, `rocketnet_server_update`,
  `rocketnet_server_client_connected`, `rocketnet_server_receive_messages`
  (the batch form of the server's one receive queue, each entry naming its
  sender and the channel it was delivered on), `rocketnet_server_send_packets`,
  `rocketnet_server_destroy`
- `rocketnet_client_default_config_sized`, `rocketnet_client_create_sized`,
  `rocketnet_client_state`, `rocketnet_client_update`,
  `rocketnet_client_start_message`, `rocketnet_client_finish_message`,
  `rocketnet_client_receive_message`, `rocketnet_client_release_message`,
  `rocketnet_client_send_packets`, `rocketnet_client_destroy`
- `rocketnet_write_dev_connect_token`
- `rocketnet_time`, `rocketnet_sleep`
