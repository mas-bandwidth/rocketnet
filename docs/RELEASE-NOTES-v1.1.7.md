# RocketNet v1.1.7

Changes compared with the `v1.1.6` tag. A point release: no configuration
field, export, status value, packet layout or protocol version changes, and
v1.0.0, v1.1.0, v1.1.1, v1.1.2, v1.1.3, v1.1.4, v1.1.6 and v1.1.7 endpoints
interoperate in both directions.

- Tag: `v1.1.7`, on the merged `main` commit.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.1.7).

## Since v1.1.6

v1.1.7 changes no library code: tests, tools and CI only.

- More unit cases on the receive side's message channels: a reliable delivery on a full table, the payload capacity at both bounds, a packet that fits the receive reservation exactly, control and unordered records in the reservation, a held reliable-sequenced message below the delivery allowance, and sequenced delivery spending the unreliable allowance.
- The server's used-token history is tested at its capacity bounds, at every slot, across eviction and expiry, against an independent model of its rules, and with a guard past each of its arrays.
- The production build's refusal of the development keys is checked to report its status and to leave nothing allocated; the production connect token writer is checked to refuse an address list out of bounds.
- The server's connect token address check is tested with near-miss addresses, the wrong address family and an IPv4 client seen through an IPv6 socket; the server's accept of a delivered payload is tested against a stale connection and a slot that is not connected; every enabled message type is tested against every wire code at both of the receiver's gates.
- The server's signature budget is tested to keep one slot per failing source and to forget every source in a fresh window; the per-client connection id and end reason accessors are tested to answer only below `max_clients`; the server's cap on a connect token's timeout is tested at zero, below and above the token's value.
- A reused client slot is tested to inherit nothing from the connection that timed out in it, and replayed handshake packets are tested to keep no slot alive; more cases cover the handshake's budgets and replies, the message record reader and writer at their limits, packet sealing at its bounds, and the client's connection attempts.
- Two new fuzz harnesses, with seeds and dictionaries: `rocketnet_fuzz_endpoint` drives the message layer's receive after decryption beside a real sending endpoint, and `rocketnet_fuzz_xdp_filter` runs the XDP filter program in user space against the AF_XDP frame reader (Linux only). `tests/fuzz/Makefile` builds them; CI and the nightly fuzz job do not run them yet (tests/fuzz/README.md).
- `scripts/vm-boot-guard` runs a test guest with a boot deadline and refuses a guest kernel command line that is too long; `tools/mutation/run_mutants.py` gains `--slice K/N` (one slice of the mutants), `--platform linux`, `--changed-since <tag>` and `--summary-md`.

## Known issues fixed in CI

- The nightly `linux / transport cells x50 and ASan` job had not completed since v1.1.4. Its XDP test guest never booted: the guest's commands, with all 13 XDP cases and their log paths, were passed on the guest kernel's command line, which came to 2079 bytes from the runner's work directory against the x86 limit of 2048, and the job then waited with no deadline. Test guests now read their commands from a script file, so the command line no longer grows with the case list or the log paths (about 650 to 770 bytes); a command line over 1900 bytes is refused by name before the guest boots; and every test guest has a 120 second boot deadline with three attempts, which `ROCKETNET_NIGHTLY_NO_TIMEOUT` does not lift. The job also runs one XDP repetition from a work directory over 100 characters long.
- The nightly `linux / mutation` job could not finish within its 120-hour limit: about 53,000 mutants at about 7 a minute. It had never completed. It now runs one slice of 90 each night, about 400 mutants and about an hour, so the whole list is covered every 90 nights. It mutates what a Linux build compiles, leaves out test code, and lists the AF_XDP files apart as not judged there; it fails only for a risk-class survivor on a line new or modified since the previous release, and reports every other survivor in the job summary. Each night's slice, rule and results are kept as a build artifact. A manual run with `mutation_full` runs the whole list with the original rule (TESTING.md).
