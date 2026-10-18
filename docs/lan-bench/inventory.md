# LAN inventory for the transport throughput and 1000-client phases

This inventory is not gathered. The document records why, and carries no
per-machine rows, no 10G list, and no switch-path table.

## Where the source is expected

The card takes its machine list from a coordinator inventory file outside the
repository. That file names, for each LAN machine, its label, ssh name, role,
shape, path and NIC. The file is not present in the job directory, the job
description does not name it, and the environment variable the stress design
names for it is unset. The only other candidate location is outside the job
directory, which the sandbox refuses to read. No machine list is therefore
available to query.

## Why the remote commands do not run

- The job runs on Darwin, not Linux. The card's step 1 requires Linux, and
  its gate is Linux-only.
- The sandbox grants no network access (classified `net=nopromise`), so no
  ssh session can reach any bench.
- The read-only commands the card names (`ethtool <if>`, `ethtool -i <if>`,
  `ip -s link`, `ip route`, and the macOS and Windows equivalents), each under
  `timeout 20`, therefore run against no machine, not even as an attempt.

## State of this file at the staged commit

The file already exists at the staged commit, written by the previous attempt
with the same untakeable result. The card's done condition requires this path
to be absent at base, so that condition cannot hold on this lineage.

No host name, address or MAC is read or recorded here. `docs/LIVE-XDP.md` is
read for the label scheme and left unchanged.
