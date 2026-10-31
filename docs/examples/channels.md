# Channels

Games put different traffic on different channels so a lost reliable message
(like chat or an inventory change) does not impose an ordering wait on
time-critical state updates (like player positions) on another channel.
Channels still share connection memory, wire budget, and packet capacity.

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
last so queued work becomes available to the transport.

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
messages in sequence once the retransmission arrives. `PASS` checks that all eight chat messages arrived in order and that any
received positions increased. The diagnostic lines illustrate stalls when
they occur; a particular stall pattern is not required for the check to pass.

<a id="public-calls-it-uses"></a>

Source: [`examples/channels.c`](../../examples/channels.c). See the
[integration guide](../USAGE.md) for shared lifecycle, threading, and message
ownership rules.
