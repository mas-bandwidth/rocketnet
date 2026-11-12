# RocketNet v1.1.2

Changes compared with the `v1.1.1` tag. A point release: no configuration
field, export, status value, packet layout or protocol version changes, and
v1.0.0, v1.1.0, v1.1.1 and v1.1.2 endpoints interoperate in both directions.

- Tag: `v1.1.2`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.2).

## Fixes since v1.1.1

- A server slot freed during an update is no longer given to a new client in that same update: it reads not connected, with the ended connection's `rocketnet_server_disconnect_reason`, after the update that frees it, and a new client can take it from the next update. A connection response that finds only such slots free is ignored rather than denied, and the client's resend connects in the next update.
- A connection the server ends at the AEAD integrity limit (data for it failed authentication 2^36 times) now reads `ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION` on the server, as the client's end of the same limit already did, instead of `NONE`.
- On Linux xdp, a stop no longer waits on a TX ring that takes no descriptor (a link whose carrier dropped) for a count of worker passes, which could run from under two seconds to well over ten: it gives up on that ring 1.0 s after the stop began or after the ring last moved, counts its frames as send failures and logs them.
- The Linux XDP kernel program now reads the UDP and IP length fields with `bpf_ntohs` instead of an unconditional byte swap, and is compiled for the target's byte order (`bpfel` or `bpfeb`) rather than the build machine's; a build whose target byte order is not known compiles XDP out. Big-endian builds read the lengths correctly; little-endian builds are unchanged.
- On the Linux gso and io_uring server transports, a destination whose route refuses a UDP segmented send (an IPsec policy, say) now has that run resent unsegmented and is remembered so its later runs go unsegmented, while every other destination keeps segmenting; io_uring no longer drops the refused run. Segmentation turns off for the whole transport only when 8 destinations refused and no segmented send was ever accepted ([TRANSPORT.md t2.4](../standard/TRANSPORT.md#t2.4), rule 7).
- LINUX.md and DEPLOYING.md now document requirement R8: on kernels before 6.12 the real-time throttle (`kernel.sched_rt_runtime_us`) stops each spinning XDP worker for about 50 ms a second, and much longer on a `nohz_full` CPU; `scripts/rocketnet-provision` now sets it to -1 and `verify` reports it unmet.
- ERRORS-AND-LOGGING.md's unreachable-client example now states when a freed slot reads not connected, and keeps its saved refusal count keyed by `rocketnet_server_client_connection_id` for a game that reads less often than every update.
- Big-endian builds are now checked per commit for the XDP filter, the embedded XDP program's byte order and the standard's byte vectors, and nightly against a little-endian end; test, tool and documentation changes only.
