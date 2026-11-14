# RocketNet v1.1.59

Changes compared with the `v1.1.58` tag. A point release: no packet layout or
protocol version change, no public API change and no new counter (the
contract still holds 292 counters), and v1.1.59 endpoints interoperate with
every v1.x endpoint.

- Server, checked API:
  - A `rocketnet_server_receive_messages` refused because another main-thread
    call was running (counted `MAIN_THREAD_CALL_OVERLAP`) emptied the batch
    the game held. A refused receive now leaves the batch and the queue as
    they were.
  - Two `rocketnet_server_stop` calls at the same time could leave the
    server reporting stopped while its transport was never polled or
    destroyed. The second stop now finds the server already stopping and
    does nothing.
  - Three more checked calls, `rocketnet_server_disconnect_client`,
    `rocketnet_server_disconnect_all_clients` and
    `rocketnet_server_set_send_kbps`, now refuse an overlapping call before
    touching any state. The calls made once per frame are recorded on the
    roadmap for the same treatment.
- Memory budget: with an operator memory budget set, a list of built-in
  transports that were all over the budget, followed by a custom transport,
  failed with `NO_TRANSPORT_FOUND` before the custom transport was tried. The
  custom transport is now tried, as it already was when listed alone.
- Server transport: asking a transport that has not been given its client
  slots for one client's send capacity read through a missing pointer. It now
  answers the transport's single send capacity.
- Windows ASan builds: freeing a block no longer commits an eighth of the
  block's size in address-sanitizer shadow memory. Other builds are unchanged.
- Documentation: `WINDOWS.md`, `ENVIRONMENT.md` and `TRANSPORTS.md` said an
  `iocp` send-thread count above the send lanes is refused at create with
  `CONFIG_OUT_OF_RANGE`. Create checks only 1 to 16; the check against the
  lanes is made at start, where `iocp` refuses and start tries the next
  transport (with `iocp` alone, start fails with `NO_TRANSPORT_FOUND`), as
  `rocketnet.h` says.
- Tests and tools: the loopback test of a client woken by its own send now
  updates the client while it waits, as a game does, so it no longer fails
  at its 1 second bound under load; the Windows memory check plans the
  `iocp` send threads the library starts; and the xdp filter fuzz target
  builds without warnings on Windows.
