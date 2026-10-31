# RNClientGroCarry: the gso client's GRO receive at the code's layout

`RNClientGroCarry.tla` models `rocketnet_client_gso_receive_batch` in
`sdk/rocketnet_client_gso_linux.cpp`: the slots one call fills, the coalesce
buffer that holds one received message, and the segment index into it. A
message the kernel coalesced is cut at its stride into segments; a call
delivers them into its slots in order while it has room, and the segments it
has no room for stay in the coalesce buffer for the next call, which delivers
them before it reads the socket again. The I/O thread above it is
`RNClientIo.tla`'s.

## The instance

`MCRNClientGroCarry.tla`: rooms 1 to 4 (`Cap = 4`), messages of up to three
segments (`MaxSeg = 3`), seven datagrams in all. `MaxSeg` below `Cap` lets one
call read a second message after a short first one, which the code allows
whenever the room left holds `ROCKETNET_CLIENT_GSO_RECEIVE_MAX_SEGMENTS`.

## Actions and the code they mirror

| Action | Code |
|---|---|
| `Arrive(k)` | the network: a message of `k` datagrams lands in the socket (environment) |
| `Begin(r)` | the call, `max_packets = r`, `total = 0` |
| `Deliver` | `rocketnet_client_gso_receive_deliver`: `slots[total] = segment carry_segment`, both incremented, while `carry_segment < carry_segments` and `total < max_packets` |
| `Read` | the `recvmmsg` of one message into the coalesce buffer, only once no segment is held, and past the first slot only while `max_packets - total >= ROCKETNET_CLIENT_GSO_RECEIVE_MAX_SEGMENTS`; the message is held from then on (`carry_segment = 0`, `carry_segments = num_segments`) |
| `End` | the call returns `total` when neither is enabled |
| `Done` | a terminal stutter once every datagram arrived and was returned |

## Invariants

| Invariant | Meaning |
|---|---|
| `Conservation` | every datagram that arrived is returned, in this call's slots, held, or in the socket, each once and in arrival order |
| `InRoom` | a call writes only the slots below its room |

## Reversed witnesses

| Config | Defect | Fails |
|---|---|---|
| `MCRNClientGroCarryBrokenDropTail.cfg` | the segments past the room are dropped when the call ends, as the code did before the carry (it counted them in `gro_packets_dropped`) | `Conservation` |
| `MCRNClientGroCarryBrokenReadOverHeld.cfg` | the socket is read while segments are held, and the new message overwrites them | `Conservation` |
| `MCRNClientGroCarryBrokenPastRoom.cfg` | the delivery loop runs while `total <= room` | `InRoom` |

Each counterexample was checked by hand against the code. The drop-tail
trace is a message of three arriving, a call with room 1 that reads it and
delivers its first segment, and the end of that call: in the code before the
carry, the loop over the segments met `total >= max_packets` at the second
and added the remaining two to `gro_packets_dropped`, and the next call read
the socket again. `test_client_gso_receive_splits_gro_by_stride` gives a call
room for four of a ten-segment message and checks that the next calls return
the other six, in order, before the datagram behind it, with one `recvmmsg`.
