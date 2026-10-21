# Channels

Games put different traffic on different channels so a lost reliable message
(like chat or an inventory change) never stalls time-critical state updates
(like player positions) that only care about the newest value.

## What it shows

1. Configure a server and client over the in-memory `loopback` transport with
   35 percent loss on outgoing client packets from
   `rocketnet_client_network_simulator`, and a 120 ms resend delay.
2. Send eight `ROCKETNET_RELIABILITY_RELIABLE_ORDERED` chat messages on channel 0 while
   sending `ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED` position updates every frame on
   channel 1.
3. Observe that when packet loss drops a chat message, later chat messages on
   channel 0 are held until the missing message is resent, while newer position
   updates on channel 1 keep arriving without waiting.
4. Verify that every chat message is delivered in exact order `1..8` and that
   position updates never go backwards.

Each frame follows the standard loop order: `update`, drain the one receive
queue, simulation, `start_message` and `finish_message`, and `send_packets`
last so queued messages leave in the same frame.

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
- `rocketnet_server_default_config`, `rocketnet_server_create`,
  `rocketnet_server_start`, `rocketnet_server_destroy`,
  `rocketnet_server_update`, `rocketnet_server_send_packets`,
  `rocketnet_server_ipv4_address`,
  `rocketnet_server_public_key`, `rocketnet_server_client_connected`,
  `rocketnet_server_receive_messages`
- `rocketnet_client_default_config`, `rocketnet_client_create`,
  `rocketnet_client_connect`, `rocketnet_client_destroy`,
  `rocketnet_client_update`, `rocketnet_client_send_packets`,
  `rocketnet_client_state`, `rocketnet_client_start_message`,
  `rocketnet_client_finish_message`, `rocketnet_client_receive_messages`,
  `rocketnet_client_network_simulator`
- `rocketnet_create_dev_connect_token` and
  `ROCKETNET_DEV_BACKEND_PUBLIC_KEY` (development library only)
- `rocketnet_status_name`, `rocketnet_time`, `rocketnet_sleep`
