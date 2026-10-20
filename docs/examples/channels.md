# Channels

Games put different traffic on different channels so a lost reliable message
(like chat or an inventory change) never stalls time-critical state updates
(like player positions) that only care about the newest value.

## What it shows

1. Configure a server and client over the in-memory `loopback` transport with
   `simulated_loss_percent = 35` on the client's outgoing packets and a 120 ms
   retransmit timeout on both ends.
2. The client sends eight `ROCKETNET_RELIABILITY_RELIABLE_ORDERED` chat messages on
   channel 0, one per frame, and a `ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED` position
   update every frame on channel 1.
3. When packet loss drops a chat message, the server's later chat messages on
   channel 0 are held until the missing message is resent, while newer position
   updates on channel 1 keep arriving without waiting.
4. The server checks that every chat message is delivered in exact order
   `1..8` and that position updates never go backwards; the run ends when all
   eight chat messages have arrived, or fails after ten seconds.

Each frame follows the standard loop order: `update`, receive and release
messages, simulate, `start_message` and `finish_message`, and `send_packets`
last so queued messages leave in the same frame.

## Run it

From the repository root, after the normal build:

```sh
./bin/example_channels
```

## What the output means

```
t=0.229s positions=1 chat=1/2 (chat delivered in order)
t=0.234s positions=2 chat=2/3 (chat delivered in order)
t=0.239s positions=3 chat=3/4 (chat delivered in order)
...
t=0.264s positions=4 chat=8/8 (chat delivered in order)
PASS
```

`positions` counts position updates received and `chat` is chat messages
received over chat messages sent. A `chat delivered in order` line prints on
each frame that delivers chat. A `chat held while positions arrive` line prints
when channel 0 is waiting for a lost chat message to be retransmitted while
position updates keep arriving on channel 1; whether one appears depends on
which packets the simulated loss drops, and the run above has none. `PASS`
means all eight chat messages arrived in order and positions never went
backwards; it does not require a hold.

## Public calls it uses

- `rocketnet_init`, `rocketnet_term`
- `rocketnet_server_dev_config_sized`, `rocketnet_server_create_sized`, `rocketnet_server_start`,
  `rocketnet_server_port`, `rocketnet_server_update`,
  `rocketnet_server_client_connected`, `rocketnet_server_receive_message`,
  `rocketnet_server_release_message`, `rocketnet_server_send_packets`,
  `rocketnet_server_destroy`
- `rocketnet_client_default_config_sized`, `rocketnet_client_create_sized`, `rocketnet_client_connect`,
  `rocketnet_client_state`, `rocketnet_client_update`,
  `rocketnet_client_start_message`, `rocketnet_client_finish_message`,
  `rocketnet_client_receive_message`, `rocketnet_client_release_message`,
  `rocketnet_client_send_packets`, `rocketnet_client_destroy`
- `rocketnet_write_dev_connect_token`
- `rocketnet_time`, `rocketnet_sleep`
