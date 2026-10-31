# RNServerMaintenanceSweep

The maintenance sweep of `rocketnet_server_transport_update`
(`sdk/rocketnet_server_transport.cpp`) at the code's layout: `N` client slots,
the cursor `maintenance_next_client` and its modulo-`N` advance, and a staging
with room for one to `MaxRoom` frames each update. Every slot is connected and
its keep-alive always due, the worst case for fairness.

A sweep visits slots in ring order from the cursor, and the cursor names each
slot before its visit. `SweepStep` is one visit:

- the staging is full: the sweep ends with the cursor on that slot, so the next
  update starts there;
- the transport refuses the start for want of a link-layer destination
  (`ROCKETNET_TRANSPORT_PACKET_UNREACHABLE`, xdp with no MAC for the address):
  the refusal is counted `SEND_NO_LINK_ADDRESS`, the slot's keep-alive stays
  due, and the cursor moves past it;
- otherwise the frame is committed and the cursor moves past the slot.

`BoundedWait` holds that every reachable slot is served within `N` updates when
each update has room for at least one frame.

## Cases

| Config | Constants | Outcome |
|---|---|---|
| `MCRNServerMaintenanceSweep.cfg` | `N = 4`, `MaxRoom = 2`, `Unreachable = {1, 2}`, `SkipUnreachable = TRUE` | pass |
| `MCRNServerMaintenanceSweepBrokenBreak.cfg` | the same, `SkipUnreachable = FALSE` | `BoundedWait` violated |

## The reversed witness

`SkipUnreachable = FALSE` treats an unreachable refusal as a full staging. TLC
starts with the cursor on slot 1 (unreachable). Each update begins at slot 1,
the start is refused, and the sweep ends with the cursor still on slot 1;
slots 0 and 3 are never visited and their `wait` climbs to 5, past `N`.

Checked against the code by hand: in `rocketnet_server_transport_update` the
loop sets `maintenance_next_client = i` before the reserve, and a NULL from
`rocketnet_server_reserve_send_frame` left the loop by `break`, so the next
update's `first_client` was the same unreachable slot. The xdp start returns
NULL with `ROCKETNET_TRANSPORT_PACKET_UNREACHABLE` for every packet to an
address with no link-layer destination, so the stop repeats every update. The
code now tests `frame.packet_id` after a NULL and continues past that slot.
`test_staged_unreachable_client_does_not_pin_sweep` in
`tests/test_transport_control.cpp` is the same scenario on the code.
