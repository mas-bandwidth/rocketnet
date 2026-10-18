# MESSAGES (RN-2, normative)

This document is the normative standard for rocketnet's message layer. A
conforming implementation MUST send and accept exactly the records and fields
specified here, and MUST use the constants in the field tables exactly as
given. The key words "MUST", "MUST NOT", "SHALL", "SHOULD", and "MAY" in this
document are to be interpreted as described in RFC 2119 and RFC 8174.

## m11 The CONFIG control record

Every connection carries exactly one CONFIG control record from each end. An
end MUST send its CONFIG record on its connection's first reliable number, so
the record is resent until acknowledged. The record is code 2 with CONTROL
set.

A receiver MUST accept a CONFIG record only when its body is exactly
`CONFIG_BYTES` (28) bytes and its kind is 1. A kind other than 1, a body that
is not 28 bytes, or a version 1 body whose reserved bytes are not zero is
`MESSAGE_BAD_CONTROL` and MUST be refused as a violation. A version the
receiver does not know reads as a body and is a mismatch, not a malformed
record.

A receiver MUST compare every field of the CONFIG body with its own values. Any
difference is a CONFIG mismatch (`ERROR_CONFIG_MISMATCH`), which MUST end the
connection. Until an end receives its peer's CONFIG, it MUST wait, and when the
wait outlasts the reliable timeout the connection MUST end with
`RELIABLE_TIMEOUT`; the peer's CONFIG may not be delivered if the peer never
sends it, and a CONFIG mismatch MUST NOT be reported for a record the end
cannot reach.

### m11.1 Wire layout

The CONFIG record is written with the record header of code 2 (flags
`0x22`: code 2 with CONTROL set, length 28, first reliable number), followed
by the 28-byte body:

```
offset  size  field
0       1     kind                  1 = CONFIG
1       1     version               1
2       1     enabled               bit r set = reliability r may be sent by the application
3       1     zero
4       4     num_channels          u32
8       4     window_messages       u32
12      4     receive_ring_bytes    u32
16      4     max_message_bytes     u32
20      4     fragment_bytes        u32, 1152
24      4     zero
```

### m11.2 Field table

| Field | Size | Value | Meaning |
|---|---|---|---|
| kind | 1 | 1 | the kind identifies this record as CONFIG; kinds other than 1 are reserved and are `MESSAGE_BAD_CONTROL` |
| version | 1 | 1 | the version of this body; a version the receiver does not know is a mismatch, not a malformed record |
| enabled | 1 | bitmask | bit r set means reliability r may be sent by the application; the default is `0xFF`, all eight reliabilities of version 1 |
| zero | 1 | 0 | MUST be zero for version 1; otherwise `MESSAGE_BAD_CONTROL` |
| num_channels | 4 | u32 | the count of channels, 1 to 65536 |
| window_messages | 4 | u32 | W, a power of two, 64 to 2^20 |
| receive_ring_bytes | 4 | u32 | B, a multiple of 16, 4096 to 2^20 |
| max_message_bytes | 4 | u32 | at most `receive_ring_bytes`, 1 to 2^20 |
| fragment_bytes | 4 | u32 | the block stride F, 1152 |
| zero | 4 | 0 | MUST be zero for version 1; otherwise `MESSAGE_BAD_CONTROL` |

The library's real default configuration is: `num_channels` 32,
`window_messages` 1024, `receive_ring_bytes` 262144, `max_message_bytes`
262144, and `fragment_bytes` 1152, with `enabled` 0xFF.

### m11.3 States

An end has exactly one CONFIG state per connection:

| State | Meaning |
|---|---|
| Waiting | the end has not yet received its peer's CONFIG; it keeps the connection open and resends its own CONFIG until acknowledged |
| Verified | the end received its peer's CONFIG and every field matched its own; the connection proceeds |
| Mismatch | the end received a CONFIG whose body was well-formed but a field differed; the connection end `ERROR_CONFIG_MISMATCH` |

A waiter whose reliable timeout passes MUST end the connection with
`RELIABLE_TIMEOUT`.

### m11.4 Vectors

Each vector is the full payload, before the transport seals it. An
implementation MUST reproduce each one from its records and parse each one back
to the same records. `v1` is the CONFIG record at the library's real defaults,
number 0. `v2`..`v8` are the mismatch vectors, each identical to `v1` except
for the one field named, at the value shown; each MUST be read and compared,
and each MUST yield a mismatch against the library's defaults.

```
vector v1  CONFIG, defaults, number 0
22 1c 00 00 00 00 01 01 ff 00 20 00 00 00 00 04 00 00
00 00 04 00 00 00 04 00 80 04 00 00 00 00 00 00 01
```

```
vector v2  CONFIG, defaults except version 2
22 1c 00 00 00 00 01 02 ff 00 20 00 00 00 00 04 00 00
00 00 04 00 00 00 04 00 80 04 00 00 00 00 00 00 01
```

```
vector v3  CONFIG, defaults except enabled 0x01
22 1c 00 00 00 00 01 01 01 00 20 00 00 00 00 04 00 00
00 00 04 00 00 00 04 00 80 04 00 00 00 00 00 00 01
```

```
vector v4  CONFIG, defaults except num_channels 16
22 1c 00 00 00 00 01 01 ff 00 10 00 00 00 00 04 00 00
00 00 04 00 00 00 04 00 80 04 00 00 00 00 00 00 01
```

```
vector v5  CONFIG, defaults except window_messages 512
22 1c 00 00 00 00 01 01 ff 00 20 00 00 00 00 02 00 00
00 00 04 00 00 00 04 00 80 04 00 00 00 00 00 00 01
```

```
vector v6  CONFIG, defaults except receive_ring_bytes 131072
22 1c 00 00 00 00 01 01 ff 00 20 00 00 00 00 04 00 00
00 00 02 00 00 00 04 00 80 04 00 00 00 00 00 00 01
```

```
vector v7  CONFIG, defaults except max_message_bytes 131072
22 1c 00 00 00 00 01 01 ff 00 20 00 00 00 00 04 00 00
00 00 04 00 00 00 02 00 80 04 00 00 00 00 00 00 01
```

```
vector v8  CONFIG, defaults except fragment_bytes 1024
22 1c 00 00 00 00 01 01 ff 00 20 00 00 00 00 04 00 00
00 00 04 00 00 00 04 00 00 04 00 00 00 00 00 00 01
```
