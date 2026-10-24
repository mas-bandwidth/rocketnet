# Reliability modes

RocketNet exposes eight reliability choices through
`enum rocketnet_reliability_t`. The public names and behavior are authoritative
in `sdk/rocketnet.h`, [USAGE.md](../USAGE.md), and
[standard/MESSAGES.md](../standard/MESSAGES.md).

| RocketNet reliability | Delivery contract |
| --- | --- |
| `UNRELIABLE` | at most once, no ordering |
| `UNRELIABLE_SEQUENCED` | at most once; only the newest sequence on its channel is delivered |
| `RELIABLE` | exactly once, without ordering |
| `RELIABLE_ORDERED` | exactly once, in order on its channel |
| `RELIABLE_SEQUENCED` | resent until acknowledged; the receiver delivers only the newest message on its channel and may drop a stale one |
| `UNRELIABLE_WITH_ACK_RECEIPT` | unreliable delivery with an ACK or loss receipt |
| `RELIABLE_WITH_ACK_RECEIPT` | reliable delivery with an ACK receipt |
| `RELIABLE_ORDERED_WITH_ACK_RECEIPT` | reliable ordered delivery with an ACK receipt |

Channels scope ordered and sequenced state. Receipt serials are returned by
`finish_message` and read through the client or server receipt call. The
endpoint-wide receive queue reports each message's channel and reliability.
RocketNet's wire format, acknowledgement encoding, resend timing, fragmentation
and memory bounds are its own contracts; source compatibility with another
library is not promised.
