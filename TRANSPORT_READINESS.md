# Transport readiness

The SDK is an advanced prerelease. Wire version 1.0.0 does not freeze its native
or managed ABI. This is a repair record for the existing #143–159 findings,
not a completed independent security audit or a production certification.

## Repairs and checks

- Checked exact-size managed token buffers, with negative checks before native
  initialization and complete managed loopback smoke on three platforms.
- Explicit production sources and public exports; no test suite in production
  archives. CI compares the shared export list on Linux, macOS and Windows.
- Strict endpoint port parsing, including IPv6 bracket and truncation cases.
- Aligned allocation with original callback/context ownership; late allocator
  replacement and termination with outstanding allocations are refused.
  Atomic-containing transports and driver queues have real C++ lifetimes.
  Allocation failure checks exercise portable constructors and cleanup using
  a deliberately non-cache-aligned allocator filled with nonzero bytes.
- Soak tests require real connections and verified traffic in both directions.
  Bench campaigns require successful children, complete valid windows, and
  independent known-rate controls; malformed/empty results cannot pass.
- Packet contexts are checked against private, single-consumption leases before
  native payload access, finish or abort. Managed payload spans require their
  owner. Tests cover metadata changes, copies, stale phases, wrong owners,
  same-frame reuse and concurrent consumption. Raw memory views still expire
  at finish/abort/update/flush/destroy; callers must respect that lifetime.
- ACK/replay history has a 4096 minimum/default and an explicit build-time budget
  up to 32768. No frame/send/ACK rate is assumed. Concurrent aliases serialize;
  delayed writers cannot overwrite newer records. Overflow reports lost history
  and continues transmission. ACK return batches cover retained state rather
  than a presumed per-frame send quota. Tests send beyond two complete history
  windows at one timestamp in both directions, then check delayed notification
  of every retained ID. Other checks cover irregular, fast and slow clocks.
- Packet-rate statistics use elapsed-time buckets, with about 4 ms boundary
  resolution. Tests cover 16 through 10000 updates and repeated frozen time.
- Counter count and bounded-value introspection are available to FFI callers.
- XDP returns follow libxdp's all-or-zero reservation contract. A pending-frame
  conservation check covers repeated zero reservations then full success. Send
  frame pressure retains the staged packet for retry; no worker calls exit.
  The contract is verified against the [upstream xsk header](https://github.com/xdp-project/xdp-tools/blob/master/headers/xdp/xsk.h).
- Reassembly geometry uses checked byte estimates and a 512 MiB component budget;
  XDP queues plus UMEM use a 4096 MiB component budget. Both are explicit build
  choices. Oversized cross-field configurations fail before allocation or XDP
  preflight activity. The former 146 GiB reassembly configuration is rejected
  without a single allocation; component budgets are not host-RAM estimates.

The existing CI matrix includes native Debug/Release on Linux/macOS/Windows,
portable and automatic driver selection, serialization, .NET 8 smoke, s390x,
ASan/UBSan/LSan, TSan, bounded simulated soak, corpus replay and standard-value
conformance. A green matrix proves those scopes only. Test executables forbid
privileged XDP commit; they do not validate privileged production traffic.

## Remaining release gates

- Privileged XDP syscall failure, frame conservation and saturation on supported
  kernels/NICs; real long-running multi-client and impaired-network campaigns.
- Size/version-aware public structs, exhaustive ABI/binding generation, a
  lifetime-owning managed layer, structured create errors and FFI-safe logging.
- Whole-transport memory estimates and operator budgets that include all
  components, kernel buffers, thread stacks, allocator overhead and application
  memory. Component limits prevent the documented oversized geometry but do not
  establish that a host can afford every configuration beneath them (#155).
- Install/export packaging and external-consumer tests; reproducible release
  dependencies, supported-platform policy, security disclosure/release process.
- Independent review of the complete repair diff. A review request sent to Rowan
  is not a review received or an approval.

Reliability, ordering, most-recent state, flows and broadcast remain the layer
above this unreliable transport. No transport hardening change implements those
features or certifies an unrun hardware/performance campaign.
