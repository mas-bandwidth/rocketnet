# RocketNet v1.1.9

Changes compared with the `v1.1.8` tag. A point release: no configuration
field, export, status value, packet layout or protocol version changes, and
v1.0.0, v1.1.0, v1.1.1, v1.1.2, v1.1.3, v1.1.4, v1.1.6, v1.1.7, v1.1.8 and
v1.1.9 endpoints interoperate in both directions. Four behaviours change,
under "Upgrading": the server's token address rule, XDP attach, a pin-only
transport inside a longer list, and the client's RTT before its first ack.
Four counters are appended.

- Tag: `v1.1.9`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.9).

## Upgrading

- **A token address must be one the server advertises.** A server created at
  the IPv6 wildcard (`"[::]"`) used to accept an IPv4 token naming
  `127.0.0.1` at its port, and no other loopback. That exception is gone: a
  token address is admitted only when it equals, port included, the
  advertised address of its family (after the usual v4-mapped
  normalisation). A server that should accept loopback tokens names the
  loopback, for example
  `rocketnet_server_create( context, "127.0.0.1:P", "[::]:P", ... )`, which
  binds the same dual-stack socket. The same exception applied to a server
  created at the IPv6 wildcard with a named IPv4 address; such a server now
  accepts only the IPv4 address it names. See
  [RN-1 t12 rule 6](../standard/TRANSPORT.md#t12).
- **XDP attaches in native mode only.** A driver without native XDP used to
  fall back silently to generic (skb) mode, the slow path. The xdp transport
  now refuses with an error naming the device: a pin on xdp makes
  `rocketnet_server_start` fail, and `auto` falls to the next transport.
- **A pin-only transport inside a longer transport list is skipped** with a
  log, as RN-1 t2.4 says, on a client and a server alike. Loopback is
  selected only as the sole name of a list.
- **The client's RTT no longer falls back to the handshake's estimate.** It is
  the minimum first-ack sample, and zero until the first one (RN-1 t15.5), as
  the server's already was.

## Since v1.1.8

### Wire-tracked edits

No packet layout changes. Files that the wire-change rule tracks by path were
edited, so this note lists every edit:

- `standard/TRANSPORT.md`: t12 rule 6 defines "advertises" (above); t12
  rule 7 names the counter for a withheld cookie reply; the client's
  stop-time receive discard is specified; the four new counters join the
  counter table and its `t18.counter-registry` vector.
- `standard/MESSAGES.md`: m9.4 states that a packed unreliable build open
  when reliable work becomes due may be aborted at its seal; its records are
  counted as `MESSAGE_UNRELIABLE_OVER_BUDGET`, a server spends no sequence
  and a client burns the one its frame took.
- `sdk/rocketnet_server_protocol.cpp`: the token address rule above, and the
  cookie reply limit's count.
- `sdk/rocketnet_client_protocol.cpp`: a keep-alive's client index and slot
  count are compared as the unsigned 32-bit fields they are.
- `sdk/rocketnet_ack.cpp`: an elapsed time at or past the RTT sentinel (a
  legal jump of the update clock) is no RTT sample; summed, it made jitter
  infinite.
- `sdk/rocketnet_wire.h`: the XDP program's configuration carries a VLAN ID
  (below); an untagged server writes the same bytes as before.
- `sdk/rocketnet_layers.h`: comments and documentation (lease misuse, RTT),
  and two development-build test seams for the link simulator's lock.
- `sdk/rocketnet.h`: comments and documentation only (loopback pin wording,
  joins spread over time).

### Library

- **Counters.** Four counters are appended: `CLIENT_RECEIVE_DISCARDED_AT_STOP`
  (220), `CONNECTION_REQUEST_COOKIE_REPLY_WITHHELD` (221),
  `XDP_REPLAY_COMMIT_FAILED` (222) and `XDP_REPLAY_GREETING_HELD` (223).
  Every earlier id, name and class is unchanged.
- **Counters are ABI.** The counter table is one table, identical on every
  platform and in every build (`ROCKETNET_DEV` on or off), never behind a
  preprocessor condition. An id is never renumbered, renamed, removed or
  reused. `tests/counter_contract.inc` lists every counter by id, name and
  class. A check refuses any change to it other than an append, and any
  change to the rows a release shipped. A test reads the built table against
  it on every platform and in both builds.
- The client's I/O worker reads and counts every datagram still queued on its
  socket when it exits, then closes the socket before the transport reports
  stopped, and closes its wake handle only after stop's wake call returns.
- A server whose transport refused one client's destination with room left
  no longer stops sending keep-alives and acks to every later connected
  client.
- The client's `packets_sent` and `bytes_sent` count a datagram only when it
  leaves, including the final datagrams sent at stop.
- A two-family client over loopback takes one hub endpoint, so a hub sized
  for N clients admits N.
- On a kernel that cannot segment, the gso and io_uring servers fall back once
  eight distinct destinations are refused, even when they share a table
  entry.
- A whole-frame in-place send dropped over budget now counts against the
  update's packet allowance.
- In a development build, a link-simulator lock miss is no longer counted as
  `SEND_RING_FULL`.
- Transport packet leases: a lease reset by update, send or stop is refused,
  counted as API misuse and logged in every build, by finish and by abort
  alike. Every other misuse asserts in a debug build and is counted in every
  build.
- XDP: a server can serve an address on one 802.1Q VLAN device. A
  colliding address's answered entry is kept while a slot is free. An
  IPv4-mapped IPv6 source is dropped, as every socket transport does. Create
  and pre-flight refuse an IPv4 link-local address. The replay writer's commit
  failures and held keep-alives are counted.
- Linux: the gso and io_uring servers share one socket open and offload probe.
  A default route that names only a nexthop object is resolved through it.
  Every socket create, bind, setsockopt and getsockname failure line names
  the OS error.
- gso client: a message past GRO's 64 segments delivers every segment, a
  single receive splits a GRO-coalesced message, and a `sendmmsg` that a
  signal interrupted is retried. The gso server reports GRO control-message
  faults.
- Windows: the bundled crypto's lock is a static SRWLOCK, so unloading the
  DLL leaves nothing behind. The IOCP client counts an empty or oversized
  datagram through the prefilter.
- `rocketnet_time` keeps the first init's origin across term and init.
  `ROCKETNET_LOG_LEVEL` is read before the platform starts.
- The light configs set `num_channels` to 8, or to `ROCKETNET_MAX_CHANNELS`
  in a build of fewer, so a build of fewer channels no longer refuses its own
  light config.
- The client's memory plan counts each I/O record by its owner and equals its
  allocation exactly.
- A heard age with no transport reads -1 instead of dereferencing NULL. The
  loopback hub's ephemeral range holds every endpoint of a full hub, and a
  contended loopback shutdown counts only real sends.

### Tests and tools

- The fixes above carry unit or functional cases that fail without them.
- The unit suite also runs at `ROCKETNET_MAX_CHANNELS=1`, each case derived
  or skipped and counted. `--run` accepts an exec row's name.
- Every counter is observed by a test on each platform, read from the build.
- `tests/load/harden_load` and `scripts/harden-*` drive server transport
  hardening loads. `client_frame_time` measures one client's per-frame cost
  on the game thread.
- The installed SDK is checked against a list of the files it should hold.
