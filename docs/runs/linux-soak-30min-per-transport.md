# Linux thirty-minute soak per transport

Five impaired Linux transport cells ran for 1800 seconds each on the same
source head. `scripts/soak-per-transport` supplied loss (5%), latency (30 ms),
jitter (20 ms), and duplication (2%) at both ends, and invoked `bin/soak`
with `--assert-flat`. The four socket cells ran concurrently on separate
loopback ports. XDP ran alone inside a virtme-ng guest on the dedicated XDP bench
machine, using only a guest-created veth interface.

- Date (`date -u`): Sun Oct 25 2026 UTC. Socket start 02:09:54, finish
  02:39:54; XDP start 01:55:53, finish 02:25:57.
- Source head for every build and cell: `720393a0aadd60ac43a8b5bcf887c7c51c6be999`
  (the land commit).
- Socket machine: `hostname` = `<socket bench>`, `uname -r` = `7.0.0-34-generic`,
  64 cores, `/proc/sys/kernel/io_uring_disabled` = `0`. All socket traffic
  used loopback; no physical NIC was exercised.
- XDP machine: `hostname` = `<XDP bench>`, `uname -r` = `6.8.0-142-generic`.
  The guest used that host's kernel, 16 vCPUs, a guest-only veth named
  `rnxdpsoak0`, and an isolated client namespace. No host uplink was bound
  or given an XDP program. `ethtool -i rnxdpsoak0` was not captured during
  the timed guest; its driver metadata is therefore unrecorded.
- Both hosts built this head with `make -j4` under the shared benchslot
  lock. Each build exited 0; the last line was `[100%] Built target test`.
  Hulk used a job-local byte-identical copy of the global benchslot script
  (SHA-256 `c46b24be9b39a349b70c7aa17735f0500e5b60603fc38d400ed93337e25071e3`),
  with the same `/tmp/benchslot.$i` flock files.

## Commands and load

From the exact-head checkout on the socket bench, these four commands were launched
concurrently. The private job's `run-socket-cell.sh` runs each existing
script under `~/bin/benchslot`, sets the listed `SOAK_SERVER_ADDRESS`, and
samples the whole host's one-minute load every ten seconds during the cell.

```sh
bash ../run-socket-cell.sh portable 40413
bash ../run-socket-cell.sh gso 40414
bash ../run-socket-cell.sh gso-plain 40415
bash ../run-socket-cell.sh io_uring 40416
```

Each wrapper supplied the shown address to its benchslot process, executed
the corresponding existing script command below, and retained that cell's
raw log. The launch lines above are the exact shell commands used; the lines
below show the cell arguments and bound ports without the load sampler:

```sh
SOAK_SERVER_ADDRESS=127.0.0.1:40413 scripts/soak-per-transport --seconds 1800 --cell portable --log-dir ../logs/portable
SOAK_SERVER_ADDRESS=127.0.0.1:40414 scripts/soak-per-transport --seconds 1800 --cell gso --log-dir ../logs/gso
SOAK_SERVER_ADDRESS=127.0.0.1:40415 scripts/soak-per-transport --seconds 1800 --cell gso-plain --log-dir ../logs/gso-plain
SOAK_SERVER_ADDRESS=127.0.0.1:40416 scripts/soak-per-transport --seconds 1800 --cell io_uring --log-dir ../logs/io_uring
```

Each socket cell has 180 load samples. The first was 2.45, the last 1.50,
and the maximum was 3.89 on 64 cores. No sampled load exceeded core count.

On the XDP bench, the complete XDP cell used the existing script and a temporary,
owned `/tmp/rnsoak-b-log` symlink pointing into the job's XDP log directory:

```sh
../benchslot -- scripts/soak-per-transport --seconds 1800 --cell xdp --log-dir /tmp/rnsoak-b-log
```

The script started virtme-ng with
`--run <home>/vm-kernels/vmlinuz-6.8.0-142-generic --memory 4G --cpus 16`
and ran `bin/soak --seconds 1800 --assert-flat` inside the guest, with server
transport `xdp` and client transport `portable`. The guest's server address
was `10.77.4.1:40412`; its clients used the far veth end in the guest's
network namespace. The symlink was removed after the guest exited.

An initial XDP invocation with `--log-dir ../logs/xdp` exited 1 before
guest boot: virtme-ng rejected the `~` in the required job directory's
absolute path as an invalid `--rwdir` parameter. No soak ran in that setup
attempt. The raw command and VM failure logs are retained separately. The
approved symlink changed only the log-directory spelling for the full retry;
the harness, impairment, 1800-second duration, and assertion were unchanged.

## Cell summaries

All five script commands exited 0 and printed `PASS`; each soak printed
`conservation ok (server and every client)` and finished with zero failures.
The counts below come from each cell's `soak finished` and `soak summary`
lines. `sent` may exceed `received` under the intended impairment; every
received payload was verified.

| Cell | Granted server/client | Iterations / connects | Sent / received / verified | Reconnects | Result |
| --- | --- | ---: | ---: | ---: | --- |
| portable | portable / portable | 1,600,154 / 60 | 3,178,010 / 3,177,851 / 3,177,851 | 59 | PASS |
| gso | gso / portable | 1,605,644 / 60 | 3,189,654 / 3,189,433 / 3,189,433 | 59 | PASS |
| gso-plain | gso / portable | 1,606,003 / 60 | 3,190,418 / 3,190,199 / 3,190,199 | 59 | PASS |
| io_uring | io_uring / portable | 1,553,398 / 60 | 3,087,246 / 3,087,086 / 3,087,086 | 59 | PASS |
| xdp | xdp / portable | 1,618,812 / 60 | 3,201,022 / 3,158,371 / 3,158,371 | 59 | PASS |

The only nonzero client disconnect reason in every summary was
`ROCKETNET_CLIENT_STATE_DISCONNECTED=60`, matching the 60 planned
connection cycles. The server's disconnect counters were:

| Cell | DISCONNECT_RECEIVED | DISCONNECT_NO_MAPPING | DISCONNECT_DISCONNECTED | CLIENT_DISCONNECTED |
| --- | ---: | ---: | ---: | ---: |
| portable | 590 | 531 | 59 | 59 |
| gso | 590 | 531 | 59 | 59 |
| gso-plain | 590 | 531 | 59 | 59 |
| io_uring | 600 | 540 | 60 | 60 |
| xdp | 590 | 531 | 59 | 59 |

## Resource start / end / peak

These are the harness's post-warmup process-resource lines. Equal fd and
thread starts and ends, and resident-memory ends no greater than starts,
satisfied `--assert-flat` in every cell.

| Cell | fds | threads | resident_bytes | library_bytes |
| --- | --- | --- | --- | --- |
| portable | 8 / 8 / 10 | 3 / 3 / 4 | 116,015,104 / 116,015,104 / 116,559,872 | 105,680,962 / 105,680,962 / 125,552,767 |
| gso | 10 / 10 / 12 | 6 / 6 / 7 | 294,494,208 / 294,494,208 / 295,047,168 | 283,739,321 / 283,739,321 / 303,611,126 |
| gso-plain | 10 / 10 / 12 | 6 / 6 / 7 | 294,494,208 / 294,494,208 / 295,038,976 | 283,739,321 / 283,739,321 / 303,611,126 |
| io_uring | 10 / 10 / 12 | 1 / 1 / 2 | 1,270,026,240 / 1,270,026,240 / 1,270,616,064 | 1,259,317,887 / 1,259,317,887 / 1,279,189,692 |
| xdp | 18 / 18 / 20 | 2 / 2 / 3 | 584,519,680 / 576,155,648 / 589,635,584 | 356,161,999 / 356,161,999 / 376,033,804 |

The five raw soak logs' SHA-256 digests, in table order, are:

```text
portable   492d946ddbddb2c4357d4a3669e15ce735ff21a6734f187334d3ba3e6fe0c182
gso        85077eb9dd84e5343868ffea9725400330e0dfe7190f8d2ecd5125a8353355d0
gso-plain  349d157a20d53ae81d151d1f101f2c8705504eb346b924f71dab0f152071a298
io_uring   7130b9c68d136734ce32acd036f07bb2b0e13ca32b58071de10540922b2cef66
xdp        f6b7817876e1eb60fa05d0e1c4bbdc39c02c08fe2ff15e068e894506888c29d1
```

## Verdict

PASS: every Linux transport cell ran its complete 1800 seconds under the
specified impairment at one source head, with verified payloads, planned
disconnects, conservation, and flat process resources. The timed XDP guest's
`ethtool` metadata remains unrecorded as noted above.
