# RocketNet v1.1.60

Changes compared with the `v1.1.59` tag. A point release: no packet layout or
protocol version change, no public API change and no new counter (the
contract still holds 292 counters), and v1.1.60 endpoints interoperate with
every v1.x endpoint.

- Build: v1.1.59 did not build with gcc 11 (Ubuntu 22.04) under `-Werror`,
  which reported the send slot in `rocketnet_server_set_send_kbps` as maybe
  uninitialized. The slot is now initialised where it is declared. Behaviour
  is unchanged: the initial value is never read.
- Proofs: the `connect_token` proof harness defines the crypto tally accessor
  the SDK now reaches its tally through, and models `sodium_is_zero`, so the
  nightly `proofs-connect_token` passes again and the private block's zero
  pad read is checked. No library code changed.
