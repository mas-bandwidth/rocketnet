# RocketNet v1.1.61

Changes compared with the `v1.1.60` tag. A point release: no packet layout or
protocol version change, no public API change and no new counter (the
contract still holds 292 counters), and v1.1.61 endpoints interoperate with
every v1.x endpoint.

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
