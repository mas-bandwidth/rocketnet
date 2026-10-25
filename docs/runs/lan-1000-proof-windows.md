# LAN 1000-client proof, Windows server (iocp, rio)

Verdict: HOLD. No cell has made a valid proof run.

Two attempts are recorded here. The first, at
1ca477339e150220f90e96bbe7c61be5b175eb51, found a runner defect: the
Windows server never received its transport pin. The second, at
e40cc6a6f4d9302363f95ea957e742efc8e503a6, carries the fix to
`scripts/lan-proof-remote`. It built that head on the server, but the
server's exclusive bench lock was not granted within any bound, so neither
cell and no pinned start was made.

## Machines

| label | role | OS / kernel | NIC model | driver | speed / duplex |
|---|---|---|---|---|---|
| windows-server-1 | server | Windows 10.0.26200 (Windows 11) | Intel Ethernet Converged Network Adapter X540-T2 | ixt65x64.sys | 10000 Mb/s Full |
| linux-client-1 | client (2 shards of 250) | Linux 7.0.0-38-generic | Aquantia AQtion AQC113 (rev 03) | atlantic | 1000 Mb/s Full |
| linux-client-2 | client (2 shards of 250) | Linux 7.0.0-38-generic | Aquantia AQtion AQC113 (rev 03) | atlantic | 1000 Mb/s Full |

These facts were read in the first attempt, on 2026-10-25. The server's facts
come from `Get-NetAdapter` through the runner's hostinfo step. The clients'
facts come from `ethtool eno1`, `ethtool -i eno1` and `lspci`. The second
attempt's hostinfo call on the server timed out in the lock queue, so it read
no new facts.

## Cells

| cell | server | nic Mb/s | clients | duration | connected | timeouts | mode delivery | size delivery | min kbps | verdict |
|---|---|---|---|---|---|---|---|---|---|---|
| iocp | windows-server-1 | 10000 | 1000 | 600 | not made | not made | not made | not made | not made | not made: the server's exclusive lock was not granted (second attempt) |
| rio | windows-server-1 | 10000 | 1000 | 600 | not made | not made | not made | not made | not made | not made: the server's exclusive lock was not granted (both attempts) |

The runner printed no table row for either cell. No merger verdict line, per-mode
or per-size verdict, receipt or per-client kbps exists for either cell.

## First attempt (head 1ca47733)

Runs between 04:14 and 05:13 UTC on 2026-10-25.

```text
scripts/lan-proof --inventory <inventory> --cell iocp --duration 600 --clients 1000 \
    --commit 1ca477339e150220f90e96bbe7c61be5b175eb51 --run-dir <dir>/iocp --run-id 1ca47733000010c1
scripts/lan-proof --inventory <inventory> --cell rio --duration 600 --clients 1000 \
    --commit 1ca477339e150220f90e96bbe7c61be5b175eb51 --run-dir <dir>/rio --run-id 1ca4773300000210
```

Start load (load1 / logical cores) was 28.16/64, 0.13/16 and 0.23/16 for iocp,
and 2.56/64, 2.01/16 and 6.26/16 for rio. Both runs were admitted.

iocp stopped with `lan-proof: FAIL: the locked Windows server ended before it
was ready`. The server log tail, with addresses redacted:

```text
info: server transport started on 0.0.0.0:40000 [portable] (development keys)
split_transport_refused: role=server run_id=1ca47733000010c1 pin=none granted=portable
split_proof: role=server run_id=1ca47733000010c1 transport=portable transport_pin=none ... disconnects=0 verdict=FAIL
```

Cause: `cmd_start_server` set `ROCKETNET_SERVER_TRANSPORT` in the Linux
subsystem shell. That shell hands a variable to a Windows program only when
WSLENV names it. `env ROCKETNET_PROBE=set cmd.exe /c echo %ROCKETNET_PROBE%`
prints the literal `%ROCKETNET_PROBE%`. With `WSLENV=ROCKETNET_PROBE` added, it
prints `set`. The server saw no pin, fell back to portable, and the proof mode
refused it, as designed. This was a runner defect, not a library result.

rio stopped with `lan-proof: FAIL: the server printed no split_ready line`. The
exclusive hold stayed queued behind other exclusive work on the server for the
runner's whole 900 s ready wait.

## The fix (head e40cc6a6)

`cmd_start_server` now passes
`WSLENV="ROCKETNET_SERVER_TRANSPORT${WSLENV:+:$WSLENV}"` with the pin. Names
already in WSLENV are kept: with WSLENV unset the value is
`ROCKETNET_SERVER_TRANSPORT`, and with `USERPROFILE/p:FOO` set it is
`ROCKETNET_SERVER_TRANSPORT:USERPROFILE/p:FOO`. Linux and macOS servers
ignore WSLENV, so their start is unchanged. `scripts/lan-proof-selftest`
prints `lan-proof-selftest: ok`.

## Second attempt (head e40cc6a6)

Runs between 05:17 and 06:33 UTC on 2026-10-25.

- Remote step script installed on the server under a shared slot. The slot
  was granted after a 10-minute queue.
- `lan-proof-remote build e40cc6a6...` was granted a shared slot after
  about 30 minutes. It built
  `bin/Release/load_split.exe` and reported head
  `e40cc6a6f4d9302363f95ea957e742efc8e503a6`.
- Server hostinfo: the 1200 s bound expired in the lock queue. No facts were
  read.
- The pinned start (iocp then rio, about 15 s each under the exclusive lock,
  logging the granted transport) queued at 06:08. Its 1500 s bound expired
  at 06:33 without the lock being granted, so it was not made. The fix is
  therefore not yet shown by a run on the server.
- The iocp cell (`scripts/lan-proof ... --cell iocp ... --commit e40cc6a6...
  --run-id e40c01006add9cc5`) started at 06:08 with the client-host lock
  held. It was stopped at 06:18, still queued on its first server step, so
  that the client hosts were not held idle. No server or shard was started.
  rio was not started.

## What closes this

1. A short pinned start on the server at this head, showing
   `[iocp]` and `[rio]` granted (not portable).
2. Both cells, when the server's exclusive lock can be granted within the
   runner's bounds. The server's queue, not the library, has blocked every
   run so far. The runner's 900 s lock wait and its 1800 s Windows build
   bound are both shorter than the queue seen in the second attempt.

The known history of raw UDP drops on the old Windows 1 GbE path is not
exercised here. The server's test adapter negotiates 10 GbE, and the client
links negotiate 1 GbE.
