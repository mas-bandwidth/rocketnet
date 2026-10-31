# Reliability modes

RocketNet exposes eight reliability choices through
`enum rocketnet_reliability_t`. The public names and behavior are authoritative
in `sdk/rocketnet.h`, [USAGE.md](USAGE.md), and
[standard/MESSAGES.md](../standard/MESSAGES.md).

The table omits the common `ROCKETNET_RELIABILITY_` enum prefix. Delivery is
scoped to one connection: reliable work is retried until acknowledged or the
connection ends. It is not durable across disconnects or process failure.

| RocketNet reliability | Delivery contract |
| --- | --- |
| `UNRELIABLE` | at most once, no ordering |
| `UNRELIABLE_SEQUENCED` | at most once; only the newest sequence on its channel is delivered |
| `RELIABLE` | resent until acknowledged; delivered once, without ordering |
| `RELIABLE_ORDERED` | resent until acknowledged; delivered once, in order on its channel |
| `RELIABLE_SEQUENCED` | resent until acknowledged; the receiver delivers only the newest message on its channel and may drop a stale one |
| `UNRELIABLE_WITH_ACK_RECEIPT` | unreliable delivery with an ACK or loss receipt |
| `RELIABLE_WITH_ACK_RECEIPT` | reliable delivery with an ACK receipt |
| `RELIABLE_ORDERED_WITH_ACK_RECEIPT` | reliable ordered delivery with an ACK receipt |

Channels scope ordered and sequenced state. Receipt serials are returned by
`finish_message` and read through the client or server receipt call. The
endpoint-wide receive queue reports each message's channel and reliability.

An ACK receipt reports acceptance by the remote library, not application processing.
Only unreliable receipt sends can report LOSS; a send promoted to reliable
reports ACKED only. A send still in flight when its connection ends gets no
terminal receipt. Already recorded outcomes remain readable until the next
update; reads do not consume them. A receipt call returns the available count,
which can exceed the supplied capacity: inspect only
`min(returned_count, capacity)` entries and reread with a larger buffer before
the next update if necessary. A full receipt ring drops its oldest outcome.
See [receipt handling](USAGE.md) and the public header for the full lifecycle.

RocketNet's wire format, acknowledgement encoding, resend timing, fragmentation
and memory bounds are its own contracts; source compatibility with another
library is not promised.
