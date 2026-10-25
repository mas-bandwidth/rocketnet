# LAN bench inventory requirements

Multi-host throughput validation is planned hardening work. The coordinator's
private machine inventory is the authority for current host identities,
allocations and addresses; this repository does not publish them.

Before a LAN result can be compared, its receipt must record for both endpoints:

- operating system and kernel version;
- physical interface, negotiated speed and duplex;
- NIC or OS driver and firmware version;
- route and address family used by the test;
- the path's known bottleneck, including switch links when available;
- source head, build type, transport selection and complete command;
- load guard before and after the run.

A NIC's advertised speed does not prove the complete path speed. Missing switch
or route evidence is recorded as unknown. Unreachable or unallocated hosts are
not results. Bench work follows the allocation and safety procedure in the
current sprint environment.

The procedure and results table for proof cells are in
[LAN proof](../LAN-PROOF.md).
