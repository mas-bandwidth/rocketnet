# LAN inventory for the transport throughput and 1000-client phases

This inventory was not gathered on this attempt (rocketnet-lan-inventory.w1).

The card's source is the coordinator's inventory file, which names the LAN
machines to query (one row per machine: label, ssh-name, role, shape, path,
and nic for server rows). That file was not present in the job environment on
this attempt, so no machine list was available to query.

The sandbox granted no network access (landlock `net=nopromise`; an ssh
resolution attempt fails with "Temporary failure in name resolution"), so
the read-only remote commands (`ethtool <if>`, `ethtool -i <if>`, `ip -s
link`, `ip route`, and the platform equivalents for macOS and Windows), each
under `timeout 20`, could not run against any machine.

Consequently this document has no per-machine rows, no 10G list, and no
switch-path table. No host name, address, or MAC was read or recorded.
docs/LIVE-XDP.md was read for the label scheme and left unchanged.
