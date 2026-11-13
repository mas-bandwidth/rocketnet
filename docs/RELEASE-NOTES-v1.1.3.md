# RocketNet v1.1.3

Changes compared with the `v1.1.2` tag. A point release: no configuration
field, export, status value, packet layout or protocol version changes, and
v1.0.0, v1.1.0, v1.1.1, v1.1.2 and v1.1.3 endpoints interoperate in both
directions.

- Tag: `v1.1.3`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.3).

## Fixes since v1.1.2

- A server no longer gives up a share of the update's send capacity for a client whose own send budget refuses its packet, or whose window cannot yet number its next message: the refusal is decided before a frame is taken, so other clients keep their sends. The standard states this in [MESSAGES.md m1.8](../standard/MESSAGES.md#m1.8) and [m9.4](../standard/MESSAGES.md#m9.4); these are rules on the sender's own scheduling, and no packet layout or meaning changes.
- A client whose send budget refuses a packet no longer skips a session sequence. A server of an earlier version still reads any skipped sequence as loss, as before.
- The server's send pass now costs one visit per client slot plus one per frame sent, however many frames one client has due; before, it walked every slot once per frame of the busiest client.
- `rocketnet_server_required_memory` grows by 8 bytes a client (8,000 at 1,000 clients); clients are unchanged.
- On Windows, a server or client create that fails at a kernel object (a socket, event, thread, completion port, RIO registration, queue or first receive post) because the machine is out of memory or commit now reports `ROCKETNET_STATUS_OUT_OF_MEMORY` and stops there. Before, the transport transport order tried every later transport and ended `ROCKETNET_STATUS_NO_TRANSPORT_FOUND`.
- On the Windows iocp server transport, each segmented (USO) send carries its own segment size, so a send still pending when the next one is posted keeps the size it was posted with. The receive coalescing (URO) control area is cleared before each receive is posted.
- On the Windows iocp client transport, a destroy that waits for posted receives to drain counts only waits in which nothing completed, so a destroy whose aborted receives arrive slowly no longer logs that they did not complete and leaks the transport while they are still arriving.
- On the Windows rio server transport, a receive completion that finds the receive bank full is now dropped and counted in production builds too, where before only a development-build assert guarded the copy; the bounded dequeue keeps it unreachable. A Windows build whose pointers are not 64 bits is now refused at compile time.
- On macOS msgx, when `sendmsg_x` refuses an unconnected (server) socket with `EINVAL` and the per-packet resend then sends every message, unconnected sockets send per packet for the rest of the process, receives stay batched, one warning says so, and no send failure is counted. This is a kernel that takes no destination per message (older macOS releases); before, the server paused batching, re-probed every 60 seconds for the life of the process, and counted one send failure and logged a warning each time though nothing was dropped.
- WINDOWS.md and MEMORY.md now state that on Windows the whole memory plan is committed at create (about 19.4 GB for a default 1000-client server, though far less is resident), so a host near its commit limit refuses the create with `OUT_OF_MEMORY`; size the page file or `max_clients` so every server and client in the process fits. WINDOWS.md also states that every client's own network thread is time-critical unless `rocketnet_thread_priority( false )` is called before init.
- AGENTS.md no longer says a disconnect and reconnect inside one server update keeps `rocketnet_server_client_connected` true: since v1.1.2 a freed slot reads not connected after the update that frees it and is not retaken before the next update. Compare `rocketnet_server_client_connection_id` per slot when the game does not read every update.
- A test run that collects no case now fails, more disconnect reasons and connection conditions are tested end to end, and more cases fail when the behaviour they name breaks; test, tool and CI changes only.
- A new per-commit check holds the 1.x C ABI (the exports, and the sizes and field offsets of the public structs) to a baseline recorded from v1.1.0. Static analysis now covers the Linux io_uring server transport, and its reviewed findings are keyed by source line so a fixed finding cannot cover a new one. The formal models were corrected to match the code; tool and CI changes only.

## Known issue

- Unreliable messages sent with receipts in volume can evict reliable messages' entries from the sender's sent map before their acks arrive. Those reliable messages are sent again; with a reliable timeout configured, a connection can end with `ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT`. Not changed in this release.
