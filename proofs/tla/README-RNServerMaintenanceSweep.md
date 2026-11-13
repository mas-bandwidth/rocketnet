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
- the transport refuses the start with room left for another reason (an address
  its socket cannot carry, the case `rocketnet_driver.h` documents for a custom
  transport): the refusal is counted `SEND_FAILED`, the slot's keep-alive stays
  due, and the cursor moves past it;
- otherwise the frame is committed and the cursor moves past the slot.

`BoundedWait` holds that every reachable slot is served within `N` updates when
each update has room for at least one frame.

## Cases

| Config | Constants | Outcome |
|---|---|---|
| `MCRNServerMaintenanceSweep.cfg` | `N = 4`, `MaxRoom = 2`, `Unreachable = {1}`, `Refused = {2}`, `SkipUnreachable = TRUE`, `SkipRefused = TRUE` | pass |
| `MCRNServerMaintenanceSweepBrokenBreak.cfg` | the same, `SkipUnreachable = FALSE` | `BoundedWait` violated |
| `MCRNServerMaintenanceSweepBrokenRefused.cfg` | the same, `SkipRefused = FALSE` | `BoundedWait` violated |

## The reversed witness

`SkipUnreachable = FALSE` treats an unreachable refusal as a full staging. TLC
starts with the cursor on slot 1 (unreachable). Each update begins at slot 1,
the start is refused, and the sweep ends with the cursor still on slot 1;
slots 0 and 3 are never visited and their `wait` climbs past `N`.
`SkipRefused = FALSE` gives the same trace with the cursor on slot 2.

Checked against the code by hand: in `rocketnet_server_transport_update` the
loop sets `maintenance_next_client = i` before the reserve, and a NULL from
`rocketnet_server_reserve_send_frame` left the loop by `break`, so the next
update's `first_client` was the same unreachable slot. The xdp start returns
NULL with `ROCKETNET_TRANSPORT_PACKET_UNREACHABLE` for every packet to an
address with no link-layer destination, so the stop repeats every update. The
code now tests `frame.packet_id` after a NULL and continues past that slot.
`test_staged_unreachable_client_does_not_pin_sweep` in
`tests/test_transport_control.cpp` is the same scenario on the code.

The refusal with room left was checked the same way: the code broke on every
NULL whose `packet_id` was not `ROCKETNET_TRANSPORT_PACKET_UNREACHABLE`, so a
start the transport refused for the address alone pinned the cursor as above.
The code now breaks only when `rocketnet_server_transport_io_refused_full`
says the room is gone, as the disconnect drain does.
`test_server_sweep_passes_a_destination_refused_with_room` in
`tests/test_unit_server_stop_spread.cpp` is the scenario on the code; before
the fix it failed its first check, client 0 served in every update.
