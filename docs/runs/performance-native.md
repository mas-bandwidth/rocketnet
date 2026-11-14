# Current X710 and Windows performance measurements

Reported measurements supplied November 13, 2026 for [Performance](../PERFORMANCE.md).
Every row uses 1000 players at 60 Hz in the space frame shape. Raw logs are
not published in this repository; this is a transcription of the measurement
owner's current result record, with control/comparison rows omitted.

Receive wait runs from input arrival to the server update that consumes it.
`late_1ms` is the share already waiting more than 1 ms before the preceding
receive call began. A low share indicates that the transport is not regularly
holding inputs for another update. It does not mean zero transit time or no
outliers.

PASS in this record means the offered rate is reached, zero starts are
refused in the measurement window, approximately 60 frames/s, 1000 players
and at least99.9% delivery. MARGINAL retains a small nonzero refusal count.
This pass rule differs from the stricter zero-loss, passing-chain AWS rule.
Failed reconciliation and actual loss remain explicit below.

## Measured builds

These Release DEV=ON builds use the measurement harness from
`tools/v1.1.x/spec-verify`, at 8699f72a6 or its stated Linux predecessor.
The recipes below are measurement trees, not full qualification of an
assembled release. Main denotes a change already integrated; hot-path denotes
a fix still awaiting integration.

| Label | Measured recipe | Tree | Integration status |
| --- | --- | --- | --- |
| XF | Base eb2ef9c8 plus XDP budget/handoff fix 9806b1f08 and harness | `6ca18052` | hot-path |
| P0 | Port-0 branch 923fdaad with XDP budget/handoff fix and harness | `ca6b1c84` | hot-path |
| GSO-RH | Base 4b9a1591a plus receive-handoff 84f80fc1e and harness | `db3a7e82` | receive fix on main |
| M-4b | Source 4b9a1591a plus Linux harness db8b73241 | `86a71db0` | io_uring path on main |
| WF | Windows receive-handoff 8c58f1946 plus harness | `b97e985c` | hot-path |

Address and host placeholders in the commands describe the measured topology;
substitute the actual hosts and addresses. Verify the granted transport,
worker count and effective workload in the ready record. The provisioning
and RSS descriptions record the measured setup, not permission to change a
shared machine or a guarantee that a later build accepts the same settings.

## Machines

### X710 pair (bare metal)

**Server: bare-metal-server.**
- CPU: AMD EPYC 9254, 24 cores, SMT off (`nosmt`).
- Kernel: Ubuntu 7.0.0-43-generic. Boot line: `irqaffinity=0 isolcpus=1-2 nohz_full=1-2 nosmt`.
- NIC: Intel X710 for 10GBASE-T, driver i40e, firmware 9.00, 10000 Mb/s. Port eno2; the server
  address SERVER_IP/24 is on VLAN eno2.2195.
- eno2 settings:
  - queues (Combined): 1 for xdp at 1 thread, 2 for every other row. Set by
    `scripts/rocketnet-provision eno2 N`, which also sets gro_flush_timeout 200000, checks smt off,
    and pins queue IRQs.
  - Recorded settings: GRO on, LRO off (fixed), rx-vlan-offload on, tx-vlan-offload on. These are measurement settings, not a deployment recommendation: the VLAN preflight may refuse receive-tag stripping. Follow the current [Linux provisioning requirements](../LINUX.md); the VLAN-specific measured settings are recorded below.
  - qdisc: mq root with fq_codel children (limit 10240p, target 5ms, interval 100ms,
    memory_limit 32Mb, ecn, drop_batch 64). net.core.default_qdisc = fq_codel.
- rmem_max/wmem_max: 25000000/1000000 on gso and io_uring rows, 4194304/4194304 on xdp rows (as found).

**Clients: bare-metal-clients.**
- CPU: AMD EPYC 9254, 48 threads (SMT on).
- Kernel: same as the server.
- NIC: X710, i40e, 10000 Mb/s. eno2 has 48 queues, and the clients use CLIENT_IP on eno2.2195.
- rmem/wmem 4194304. qdisc mq + fq_codel.

### Windows pair (AWS us-east-1a, VM to VM on private addresses)

**Server: m7i.16xlarge.**
- CPU: Intel Xeon Platinum 8488C, 64 logical processors.
- OS: Windows Server 2025 Datacenter, 10.0.26100.
- Adapter 'Ethernet 2': Amazon ENA, driver 2.11.0.0, private address WINDOWS_SERVER_IP. The adapter
  reports LinkSpeed 5.0 Gbps, but rows measure 9.21 Gb/s transmitted, so that figure is not the cap.
- RSS as found and restored after each plan: base 2, max processor 12, 6 processors, 6 queues. Each
  row sets RSS to its layout (below).
- The "VPC private addresses" inbound rule covers UDP 40000 and 40001.

**Players: c8gn.8xlarge.**
- CPU: arm64 Neoverse-V2, 32 vCPU.
- Kernel: Ubuntu 7.0.0-1016-aws.
- NIC: ENA ens34, 8 queues, private address WINDOWS_CLIENT_IP.
- rmem/wmem 4194304. qdisc mq + fq_codel.

## Commands

Every row is one `scripts/harden-pair` run from the controller, 60 s measured. It starts
`harden_load server` on the server host, waits for its ready line, then starts `harden_load clients`
on the client host. It reads NIC counters 20 s apart in the measured window.

### X710 rows

```
scripts/harden-pair --ssh "ssh -i <key> -o IdentitiesOnly=yes -o BatchMode=yes" \
  --server-host ubuntu@<bare-metal-server> --client-host ubuntu@<bare-metal-clients> --server-ip SERVER_IP \
  --remote <server build dir> --client-remote <client build dir> --out <cell> \
  --transport <xdp|gso|io_uring> --row space --server-dev eno2 --client-dev eno2.2195 \
  --seconds 60 --clients 1000 --threads 32 \
  --server-args "<server args>" --client-args "--input-bytes 256 --input-stamp 40001" \
  [--server-sudo]      # xdp rows
```

Server args, by transport:

- xdp at 1 thread (eno2 1 queue; XDP on CPU 1, main on CPU 2):
  `--frame-shape space --game-threads 22 --pin-first-cpu 2 --space-send wide --space-update split
  --space-net task --space-split top --game-load <0|0.5> --world-ms <0|10> --load <peak|average>
  --input-stamp 40001`
- xdp at 2 threads (eno2 2 queues; XDP on CPUs 1-2, main on 3): the same with `--game-threads 21
  --pin-first-cpu 3 --num-xdp-threads 2`.
- io_uring (2 workers on CPUs 1-2, main on 3) and gso (1 worker on CPU 1, main on 3): the same with
  `--game-threads 21 --pin-first-cpu 3`.
- Game load: "no game" is `--game-load 0 --world-ms 0`; "half game" is `--game-load 0.5 --world-ms 10`.

### Windows rows

Every row sets the server environment `ROCKETNET_SERVER_BIND_IPV4_ADDRESS=WINDOWS_SERVER_IP`. The command:

```
scripts/harden-pair --ssh "ssh -i <key> ..." --server-host Administrator@<server> --client-host ubuntu@<players> \
  --server-ip WINDOWS_SERVER_IP --server-os windows --remote <server build dir> --client-remote <players build dir> \
  --transport <rio|iocp> --row space --client-dev ens34 --seconds 60 --clients 1000 --threads 32 \
  --server-args "--frame-shape space --game-threads <T> --pin-first-cpu <F> --space-send wide \
     --space-update split --space-net task --space-split top --game-load <0|0.5> --world-ms <0|10> \
     --load <peak|average> --input-stamp 40001" \
  --client-args "--input-bytes 256 --input-stamp 40001" --server-env <each env below>
```

Layouts:

| layout | env (besides the bind address) | RSS on 'Ethernet 2' | T / F |
|---|---|---|---|
| rio 6 lanes (lane l: socket on processor 2l, worker on 2l+1) | `ROCKETNET_SERVER_NUM_RIO_LANES=6` | base 0, max 10, 6 queues | 52 / 12 |
| rio 5 lanes (built-in default) | none | base 0, max 8, 5 queues | 54 / 10 |
| iocp default placement (workers on 0 and 2, send threads after them) | none | base 0, max 2, 2 queues | 56 / 8 |
| iocp pinned | `ROCKETNET_TEST_IOCP_LAYOUT=2,4/3,5` | base 2, max 4, 2 queues | 54 / 10 |

## Workload

- 1000 players at 60 Hz.
- **Peak**: 15 messages of 1200 B per player per tick, 900k packets/s offered.
- **Average**: 480k packets/s offered.
- Players send a 256 B input every tick, stamped on the server clock. The offset comes from a UDP echo
  on port 40001.
- "no game": game load 0, world 0 ms. "half game": game load 0.5, world 10 ms.

## Result columns

- **pkts/s** and **Gb/s** are measured server transmit, from NIC counters (X710 `port.tx_bytes`;
  Windows Get-NetAdapterStatistics).
- **lost** is s2c/c2s messages lost.
- **wait** is the receive delay in ms (mean, p99).
- **work** is main-thread work per update in ms (p50/p99).
- A range is min-max over the runs.

---

## AF_XDP

### xdp at 1 thread, eno2 1 queue

| head | status | load | game | runs | verdict | pkts/s | Gb/s | refused | overruns | lost | wait mean | wait p99 | late_1ms | work p50/p99 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| P0 | hot-path | peak | half | 2 | PASS 2/2 | 900.3k | 9.26-9.27 | 0 | 0 | 0/0 | 5.2-10.0 | 17.0-19.0 | 0.18-0.34% | 0.43-0.44 / 0.47-0.57 |
| XF | hot-path | peak | half | 3 | PASS 3/3 | 900.3k | 9.26-9.27 | 0 | 0 | 0/0 | 6.6-8.3 | 14.0-19.5 | 0.15-0.39% | 0.42-0.44 / 0.47-0.63 |
| XF | hot-path | peak | none | 3 | PASS 3/3 | 900.3k | 9.27 | 0 | 0 | 0/0 | 8.9-11.6 | 15.8-16.7 | 0.17-0.44% | 0.43 / 0.45-0.46 |
| XF | hot-path | average | half | 3 | PASS 3/3 | 480.3k | 4.94 | 0 | 0 | 0/0 | 4.3-10.4 | 10.5-17.0 | 0.06-0.44% | 0.38-0.44 / 0.45-0.47 |
| XF | hot-path | average | none | 3 | PASS 3/3 | 480.3k | 4.94 | 0 | 0 | 0/0 | 5.7-8.1 | 12.3-16.7 | 0.10-0.18% | 0.42-0.43 / 0.44-0.45 |

The XDP receive-handoff fix is awaiting hot-path integration. The XF and P0
rows measure that fix; no assembled release result is implied.

### xdp at 2 threads, eno2 2 queues

| head | status | load | game | runs | verdict | pkts/s | Gb/s | refused | overruns | lost | wait mean | wait p99 | late_1ms | work p50/p99 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| XF | hot-path | peak | half | 3 | PASS 3/3 | 900.3k | 9.26-9.27 | 0 | 0 | 0/0 | 9.8-10.9 | 17.0-19.3 | 0.15-0.38% | 0.43-0.44 / 0.48-0.62 |
| XF | hot-path | peak | none | 3 | PASS 3/3 | 900.3k | 9.27 | 0 | 0 | 0/0 | 9.8-10.7 | 16.2-16.7 | 0.17-0.41% | 0.43 / 0.45-0.47 |
| XF | hot-path | average | half | 3 | PASS 3/3 | 480.3k | 4.94 | 0 | 0 | 0/0 | 6.0-11.2 | 17.5-18.0 | 0.09-0.46% | 0.42-0.44 / 0.50-0.55 |
| XF | hot-path | average | none | 3 | PASS 3/3 | 480.3k | 4.94 | 0 | 0 | 0/0 | 7.5-8.4 | 13.5-16.7 | 0.17-0.24% | 0.42-0.43 / 0.45 |

## io_uring (2 workers, eno2 2 queues, rmem 25000000 / wmem 1000000)

| head | status | load | game | runs | verdict | pkts/s | Gb/s | refused | overruns | lost | wait mean | wait p99 | late_1ms | work p50/p99 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| M-4b | main | peak | half | 3 | PASS 3/3 | 900.1-900.2k | 9.26-9.27 | 0 | 1-3 | 30-195/0 | 6.2-10.4 | 17.2-19.2 | 0.31-0.33% | 0.41-0.43 / 0.47-0.51 |
| M-4b (qdisc test, fq_codel) | main | peak | half | 6 | PASS 6/6 | 900.1-900.3k | 9.26-9.27 | 0 | 0-3 | 0-210/0 | 7.0-11.9 | 18.3-20.0 | 0.17-0.34% | 0.39-0.42 / 0.48-0.58 |
| M-4b | main | peak | none | 3 | PASS 3/3 | 900.3k | 9.27 | 0 | 0 | 0/0 | 4.9-12.2 | 11.3-17.5 | 0.22-0.27% | 0.42 / 0.44-0.46 |
| M-4b | main | average | half | 3 | PASS 3/3 | 480.3k | 4.94 | 0 | 2-4 | 0/0 | 8.4-11.7 | 13.8-18.7 | 0.19-0.41% | 0.42-0.43 / 0.45-0.51 |
| M-4b | main | average | none | 3 | PASS 3/3 | 480.3k | 4.94 | 0 | 0 | 0/0 | 10.3-11.5 | 16.2-17.3 | 0.25-0.37% | 0.41 / 0.44-0.45 |

The measured io_uring rows reach the offered rate with a low late-input share
(0.17–0.41%), consistent with no regular extra-update hold while retaining
the reported tail. At peak with half game, fq_codel drops
whole 15-packet GSO skbs as sojourn drops. 7 of 9 fq_codel rows lost 15-210 s2c messages (at most
0.0004% of the 54M sent). In one qdisc-test run the NIC capture was missed, so its Gb/s is not in
the range. The pfifo comparison is pending (QDISC2).

The row owner reports failing reconciliation chains for GSO and io_uring,
including retry accounting limitations. Preserve that failed-chain status
separately from the table's rate/refusal/player-delivery verdict. The io_uring
losses above remain actual reported loss; the accounting limitation does not
make those rows lossless.

## rio (Windows pair)

| head | status | layout | load | game | runs | verdict | pkts/s | Gb/s | refused | overruns | lost | wait mean | wait p99 | late_1ms | work p50/p99 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| WF | hot-path | 6 lanes | peak | half | 3 | PASS 3/3 | 900.3k | 9.21 | 0 | 0-1 | 0/0 | 6.9-9.3 | 21.0-25.2 | 1.05-2.72% | 0.53 / 0.75-0.82 |
| WF | hot-path | 6 lanes | peak | none | 3 | PASS 3/3 | 900.3k | 9.21-9.22 | 0 | 0 | 0/0 | 5.5-11.1 | 16.7-17.2 | 0.21-0.34% | 0.52-0.53 / 0.57-0.63 |
| WF | hot-path | 6 lanes | average | half | 3 | PASS 3/3 | 480.3k | 4.91-4.92 | 0 | 0 | 0/0 | 8.9-9.6 | 17.7-18.8 | 0.07-0.12% | 0.52 / 0.59-0.69 |
| WF | hot-path | 6 lanes | average | none | 3 | PASS 3/3 | 480.3k | 4.91 | 0 | 0 | 0/0 | 7.6-9.2 | 14.3-16.8 | 0.02-0.10% | 0.52 / 0.54-0.55 |

Six lanes meet the rate/refusal criterion in these rows. On the receive-fix
rows, RIO_SEND_WAITS is zero except loaded peak (1–53); lane workers use
63–83% of a core at peak. The five-lane default has been placement-sensitive
on this rig; do not infer five-lane qualification from these six-lane rows.

---

## gso (1 worker, eno2 2 queues, rmem 25000000 / wmem 1000000)

The receive-handoff fix is on main, so these rows are the current state of main's gso path.

| head | status | load | game | runs | verdict | pkts/s | Gb/s | refused | overruns | lost | wait mean | wait p99 | late_1ms | work p50/p99 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| GSO-RH | main | peak | half | 3 | PASS 3/3 | 900.3k | 9.26-9.27 | 0 | 0 | 0/0 | 5.9-12.3 | 13.8-19.3 | 0.26-0.39% | 0.42-0.43 / 0.45-0.53 |
| GSO-RH | main | peak | none | 3 | PASS 3/3 | 900.3k | 9.27 | 0 | 0 | 0/0 | 9.3-12.3 | 15.8-17.7 | 0.30-0.53% | 0.42 / 0.44-0.46 |
| GSO-RH | main | average | half | 3 | PASS 3/3 | 480.2-480.3k | 4.94 | 0 | 0-5 | 0/0 | 7.9-11.2 | 16.5-18.7 | 0.11-0.37% | 0.40-0.42 / 0.44-0.52 |
| GSO-RH | main | average | none | 3 | PASS 3/3 | 480.3k | 4.94 | 0 | 0 | 0/0 | 7.7-10.6 | 17.3-17.5 | 0.12-0.23% | 0.41 / 0.43-0.44 |

## iocp (Windows pair)

| head | status | layout | load | game | runs | verdict | pkts/s | Gb/s | refused | overruns | lost | wait mean | wait p99 | late_1ms | work p50/p99 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| WF | hot-path | default | peak | half | 3 | MARGINAL | 898.4-899.4k | 9.19-9.20 | 18,663-56,479 (to 0.10%) | 0 | 0/0 | 7.5-8.8 | 18.3-19.5 | 0.18-0.48% | 0.64-0.68 / 0.78-0.85 |
| WF | hot-path | default | peak | none | 3 | MARGINAL | 900.0-900.3k | 9.21 | 1,558-8,999 (to 0.02%) | 0 | 0/0 | 7.2-10.6 | 16.8-17.2 | 0.20-0.46% | 0.64-0.68 / 0.76-0.77 |
| WF | hot-path | default | average | half | 3 | PASS 3/3 | 480.2-480.3k | 4.91 | 0 | 0 | 0/0 | 8.5-9.1 | 18.5-19.3 | 0.20-0.40% | 0.57-0.60 / 0.83-0.93 |
| WF | hot-path | default | average | none | 3 | PASS 3/3 | 480.3k | 4.91 | 0 | 0 | 0/0 | 4.6-10.1 | 16.3-17.0 | 0.16-0.33% | 0.52-0.60 / 0.57-0.68 |


---

## AF_XDP on a VLAN server: rx-vlan-offload (XDPVLAN)

XV = main e0f1a0fae + fix/v1.1.x/xdp-vlan-offload 7a2b24365 + harness, tree f81a5f26. Status: hot-path.
It carries the XDP filter change (destination MAC ff:ff and IP version checks) and the xdp
pre-flight and provision work (R2/R3/R4/R8).

The rows use one XDP thread and one eno2 queue, with the server address
on VLAN eno2.2195. They cover receive VLAN offload enabled and disabled.

**Pre-flight starts:**
- **XV, offload on:** refuses to start (one probe run):
  `xdp pre-flight: eno2 strips the 802.1Q tag on receive (rx-vlan-offload on), so the filter cannot tell VLAN 2195's frames from other VLANs' (R2): run 'ethtool -K eno2 rxvlan off', or scripts/rocketnet-provision`,
  then `xdp pre-flight failed: this machine is not provisioned for the XDP data plane`.
- **XV, offload off:** starts.

| head | status | offload | load | game | runs | verdict | pkts/s | Gb/s | refused | overruns | lost | wait mean | wait p99 | late_1ms | work p50/p99 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| XV | hot-path | off | peak | half | 3 | PASS 3/3 | 900.3k | 9.26-9.27 | 0 | 0 | 0/0 | 22.7-24.9 | 31.8-36.0 | 84.6-99.8% | 0.42-0.44 / 0.46-0.57 |
| XV | hot-path | off | peak | none | 3 | PASS 3/3 | 900.3k | 9.27 | 0 | 0 | 0/0 | 25.5-27.6 | 33.3 | 85.0-86.9% | 0.43 / 0.45-0.46 |
| XV | hot-path | on | peak | half | 1 | refuses to start (pre-flight R2) | - | - | - | - | - | - | - | - | - |

XV reaches the peak rate with receive VLAN offload disabled, but its
84.6–99.8% late-input share shows the receive delay remains. This recipe does
not include the receive-handoff fix. Qualification of the combined VLAN and
receive-handoff changes requires a measurement of their assembled tree.

## Qualification still required

The assembled hot-path tree still needs its complete measurement. The row
owner also has pending port-0, send-queue, io_uring qdisc and XDP
replay checks. These rows qualify only their measured recipes.
