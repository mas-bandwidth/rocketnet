# LAN 1000-client proof, Windows server (iocp, rio)

Date (UTC): 2026-10-25, runs between 04:14 and 05:13.
Head: 1ca477339e150220f90e96bbe7c61be5b175eb51 (built and reported by every host).
Verdict: HOLD. No cell made a valid proof run. iocp ran unpinned and was
refused, and rio never got the server's exclusive lock within the runner's
900 s ready wait.

## Machines

| label | role | OS / kernel | NIC model | driver | speed / duplex |
|---|---|---|---|---|---|
| windows-server-1 | server | Windows 10.0.26200 (Windows 11) | Intel Ethernet Converged Network Adapter X540-T2 | ixt65x64.sys | 10000 Mb/s Full |
| linux-client-1 | client (2 shards of 250) | Linux 7.0.0-38-generic | Aquantia AQtion AQC113 (rev 03) | atlantic | 1000 Mb/s Full |
| linux-client-2 | client (2 shards of 250) | Linux 7.0.0-38-generic | Aquantia AQtion AQC113 (rev 03) | atlantic | 1000 Mb/s Full |

The server's facts come from `Get-NetAdapter` through the runner's hostinfo
step. The clients' facts come from `ethtool eno1`, `ethtool -i eno1` and `lspci`.

Start load (load1 / logical cores): iocp run 28.16/64, 0.13/16, 0.23/16; rio
run 2.56/64, 2.01/16, 6.26/16. Every value was under half the cores, so both
runs were admitted.

## Commands

```text
scripts/lan-proof --inventory <inventory> --cell iocp --duration 600 --clients 1000 \
    --commit 1ca477339e150220f90e96bbe7c61be5b175eb51 --run-dir <dir>/iocp --run-id 1ca47733000010c1
scripts/lan-proof --inventory <inventory> --cell rio --duration 600 --clients 1000 \
    --commit 1ca477339e150220f90e96bbe7c61be5b175eb51 --run-dir <dir>/rio --run-id 1ca4773300000210
```

## Cells

| cell | server | nic Mb/s | clients | duration | connected | timeouts | mode delivery | size delivery | min kbps | verdict |
|---|---|---|---|---|---|---|---|---|---|---|
| iocp | windows-server-1 | 10000 | 1000 | 600 | not made | not made | not made | not made | not made | FAIL: server unpinned, refused before ready |
| rio | windows-server-1 | 10000 | 1000 | 600 | not made | not made | not made | not made | not made | not made: exclusive lock not granted within the 900 s ready wait |

The runner printed no table row for either cell, because each one stopped
before its shards started. No merger verdict line, per-mode or per-size
verdict, receipt or per-client kbps exists for either cell.

### iocp: log tail

The runner printed `lan-proof: FAIL: the locked Windows server ended before
it was ready`. The server log tail (addresses redacted):

```text
info: server transport started on 0.0.0.0:40000 [portable] (development keys)
split_transport_refused: role=server run_id=1ca47733000010c1 pin=none granted=portable
split_receipt: ... connected=0 ... transport=portable ... delivery_verdict=FAIL load_verdict=FAIL ... verdict=FAIL transport_pin=none
split_proof: role=server run_id=1ca47733000010c1 transport=portable transport_pin=none ... disconnects=0 verdict=FAIL
```

Cause: `scripts/lan-proof-remote` `cmd_start_server` sets
`env ROCKETNET_SERVER_TRANSPORT=<cell>` in the WSL shell and then runs
`load_split.exe`. WSL hands a Linux environment variable to a Win32 process
only when WSLENV names it. On the server, under its bench lock:
`env ROCKETNET_PROBE=set cmd.exe /c echo %ROCKETNET_PROBE%` prints the literal
`%ROCKETNET_PROBE%`. With `WSLENV=ROCKETNET_PROBE` added, it prints `set`.
The server therefore saw no pin, fell back to portable, and the proof mode
refused it, as designed. This is a runner defect. It is not a library result.

### rio: log tail

```text
host ... load1=2.56 nproc=64   (admitted)
lan-proof: FAIL: the server printed no split_ready line
```

`server.start` was empty. The exclusive hold-server stayed queued behind
other cards' exclusive Windows runs for the runner's whole ready wait. The
queued hold was then ended before it could start a server with no clients.
The rio server never started, so this cell's run was not made. It goes
through the same `cmd_start_server` path as iocp, but no rio run shows that.

## What closes this

1. Pass the pin through WSL in `scripts/lan-proof-remote` `cmd_start_server`,
   for example `env ROCKETNET_SERVER_TRANSPORT="$transport"
   WSLENV="ROCKETNET_SERVER_TRANSPORT${WSLENV:+:$WSLENV}" ...`. Apply this on
   Windows only, or unconditionally, since Linux and macOS ignore WSLENV.
2. Rerun both cells when the server's exclusive lock is free. The 900 s
   ready wait counts the lock queue.

The known history of raw UDP drops on the old Windows 1 GbE path is not
exercised here. The server's test adapter now negotiates 10 GbE, and the
client links negotiate 1 GbE.
