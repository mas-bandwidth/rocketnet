# RocketNet v1.1.62

Changes compared with the `v1.1.61` tag. A point release: no packet layout or
protocol version change, no public API change and no new counter (the
contract still holds 292 counters), and v1.1.62 endpoints interoperate with
every v1.x endpoint.

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
