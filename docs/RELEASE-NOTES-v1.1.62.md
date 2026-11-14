# RocketNet v1.1.62

Changes compared with the `v1.1.60` tag. A point release: no packet layout or
protocol version change, no public API change and no new counter (the
contract still holds 292 counters), and v1.1.62 endpoints interoperate with
every v1.x endpoint.

v1.1.61 was labelled on `main` but never tagged or published. Its changes,
described in [the v1.1.61 notes](RELEASE-NOTES-v1.1.61.md) and repeated
below, ship for the first time in v1.1.62.

From v1.1.61:

- Server memory: every transport now sizes its buffers from the configured
  `max_clients`, with no 512-player floor. An 8-player server on the
  portable transport needs 21.6 MB, down from 47.1 MB. Sizes at 512 players
  and above are unchanged. A few buffers keep a small minimum of their own
  slots, listed in `MEMORY.md`.
- macOS server memory: the msgx transport's receive banks, socket table and
  worker sort buckets now follow the configured `max_clients` too. Before,
  the banks were sized for the build's client maximum and the table and
  buckets for a fixed 1000 slots, so a small msgx server holds less memory
  and a 32768-client build no longer carries 2.3 GB of banks.
- Tests, tools and documentation: a functional case whose server fails to
  create or start is now red, and a transport the host cannot run is a named
  skip; `LINUX.md`, `WINDOWS.md`, `TRANSPORT-INTERNALS.md`, the backend
  example README and the transport standard now say what the code does
  (msgx is not the portable server on Linux; a zero `num_iocp_send_threads`
  selects the default). No library behaviour changed.
- Proofs: nine TLA+ models and their READMEs now describe the code as it
  is, and each check that could not fail now has a broken variant that fails
  it. No library code changed.
- Proofs: the channel, resend timing and sequencing models now match the
  channel code, models the code no longer follows are marked historical, and
  the code comments name the models they implement. No library behaviour
  changed.

New in v1.1.62:

- Sends after an ICMP error: a connected UDP socket hands back the ICMP
  error an earlier datagram drew on its next send call, sending nothing.
  The Linux gso client took that as a refusal and dropped the whole
  coalesced message (in practice the frame to the server); the portable
  sendmmsg and per-packet sends and the macOS msgx batch send dropped one
  datagram or chunk. Each now sends the same message again once; a second
  error in a row drops that one message, counted as before. On macOS a
  pending error no longer pauses the msgx send pump. Unconnected sockets
  and sends that do not fail are unchanged.
- Tests: the thread placement and pin refusal functional cases set the
  `ROCKETNET_SERVER_TRANSPORT` pin aside and check the server is on the
  transport they name, so the nightly portable leg no longer crashes them.
  No library behaviour changed.
- Tests: the macOS oversize receive case resends its loopback datagram
  inside its watchdog, so one lost datagram no longer fails it, and a
  timeout prints the cause. No library behaviour changed.
- Tests: the receive latency rows keep their clients updating until two
  updates have begun after the server's last send, so a stalled server
  thread no longer shows as lost messages. No library behaviour changed.
- Tests and tools: the channel resend timing case applies probe pacing as
  the model does, and the channel seed sweep reports the failing check by
  name. No library behaviour changed.
- Proofs: the trace checker's planted violations are held to the records
  they change, the client decrypt model treats a forged record update as
  already received, and the nightly TLC budgets and debt rows are
  re-measured. No library code changed.
- Proofs: the server connection, disconnect burst ring, scheduler, fault
  injector, destroy wipe, cookie reply, free stack and connection memory
  models, their READMEs and the coverage table now match the code, with
  reversed witnesses for the new checks. Code comments only in the library.
- Tests and tools: the deterministic simulation, load, soak, fuzz and
  source-audit harnesses now fail where they used to pass silently (outage
  rows, missing newest sends, role verdicts, counter blocks). No library
  behaviour changed.
- Proofs: the connection memory model's comments name the connection's own
  outbound CONFIG as an exhaustion site, as its README does, with its
  records rerun.
