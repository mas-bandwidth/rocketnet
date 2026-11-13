# RocketNet v1.1.33

Changes compared with the `v1.1.32` tag. A point release: no configuration,
packet layout or protocol version change, and v1.1.33 endpoints interoperate
with every v1.x endpoint.

- Sending from game threads: a server may be sent to from several threads at
  once. One main thread makes the server's calls, and between one main thread
  call and the next the game threads may call `rocketnet_server_start_message`,
  `rocketnet_server_finish_client_message` and
  `rocketnet_server_abort_client_message`, one thread per client. A server
  that never calls the finish or abort functions is used one thread at a time
  as before, and nothing is checked or counted. The rules are in
  `sdk/rocketnet.h`.
- Counters: three new counters, ids 278 to 280: `MAIN_THREAD_CALL_OVERLAP`
  and `CLIENT_SEND_TWO_THREADS` count calls refused under those rules, and
  `SEND_RESERVE_REFUSED_TAILS_HELD`. Ids 0-277 are unchanged.

- Tag: `v1.1.33`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.33).
