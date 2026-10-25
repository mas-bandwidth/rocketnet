# LAN 1000-client proof, Windows server (iocp, rio)

Verdict: HOLD. No cell has made a valid proof run.

Three attempts are recorded here. The first, at
1ca477339e150220f90e96bbe7c61be5b175eb51, found a runner defect: the
Windows server never received its transport pin. The second, at
e40cc6a6f4d9302363f95ea957e742efc8e503a6, carried the fix to
`scripts/lan-proof-remote`, but the server's exclusive bench lock was never
granted within its bounds. The third, at
8f168e8e9a11a68d5c9245b60beeb153904ccf62, showed the fix working: the pinned
server is granted `iocp` and `rio`. It also fixed a second runner defect, in
the client transport given to the merger. The iocp cell then ran with
`connected=0`, because the server host's firewall drops the clients'
inbound UDP. The rio cell was stopped before any shard started, because
the client-host lock was held by other work.

## Machines

| label | role | OS / kernel | NIC model | driver | speed / duplex |
|---|---|---|---|---|---|
| windows-server-1 | server | Windows 10.0.26200 (Windows 11) | Intel Ethernet Converged Network Adapter X540-T2 #2 | ixt65x64.sys | 10000 Mb/s Full |
| linux-client-1 | client (2 shards of 250) | Linux 7.0.0-38-generic | Aquantia AQtion AQC113 NBase-T (rev 03) | atlantic | 1000 Mb/s Full |
| linux-client-2 | client (2 shards of 250) | Linux 7.0.0-38-generic | Aquantia AQtion AQC113 NBase-T (rev 03) | atlantic | 1000 Mb/s Full |

The third attempt read these facts again on 2026-10-25 through the runner's
hostinfo step: `Get-NetAdapter` on the server's test adapter, and `ethtool eno1`,
`ethtool -i eno1` and `lspci` on the clients. The server's test adapter is
named in the inventory. The server's default-route adapter has no IPv4
address, so `default` cannot be used to name it.

## Cells

| cell | server | nic Mb/s | clients | duration | connected | timeouts | mode delivery | size delivery | min kbps | verdict |
|---|---|---|---|---|---|---|---|---|---|---|
| iocp | windows-server-1 | 10000 | 1000 | 600 | no (0 of 1000 ever connected) | 0 | unknown | unknown | 0 | FAIL: no client reached the server; its inbound UDP is blocked by the host firewall (third attempt) |
| rio | windows-server-1 | 10000 | 1000 | 600 | not made | not made | not made | not made | not made | not made: stopped before any shard started, because the client-host lock was held by other work (third attempt) |

Neither cell produced a runner table row. For iocp the row step refused, with
`cell-row: a proof field is missing`, because the merger stopped first.
The merger's verdict line for iocp was
`split_proof_cell: verdict=FAIL reason=startup_complete:_expected_1,_got_0`.
It printed no per-mode or per-size verdicts, no receipt check and no
per-client kbps. The server's own proof line had `disconnects=0`,
`min_client_up_kbps=0.000000` and `verdict=FAIL`.

## Third attempt (head 8f168e8e)

Runs between 07:37 and 09:36 UTC on 2026-10-25. That head is main with the
WSLENV fix. Every run used the remote script and the build at that head. The
merger fix below was committed afterwards, and it changes only the runner's
merge step.

### Pinned start

`load_split.exe` was built at the head in a shared slot
(`bin/Release/load_split.exe`, head reported). Then, in one exclusive hold,
it started the server for five seconds per transport with
`ROCKETNET_SERVER_TRANSPORT` set and WSLENV naming it. The server log, with
addresses redacted:

```text
iocp transport: 1 completion workers and one TX worker on port <port>, USO yes, URO yes
server transport started on <addr>:<port> [iocp] (development keys)
split_proof: role=server ... transport=none transport_pin=iocp ... verdict=FAIL
rio transport: port <port>, one I/O worker, 4096 receive slices, 2048 send frames per phase, 18087936 registered bytes
server transport started on <addr>:<port> [rio] (development keys)
split_proof: role=server ... transport=none transport_pin=rio ... verdict=FAIL
```

The pin reaches the server, and both native transports are granted, not
portable. The `verdict=FAIL` lines are expected: this start had one client
slot and no clients. After the hold, no `load_split.exe` was left running.

### iocp cell

```text
scripts/lan-proof --inventory <inventory> --cell iocp --duration 600 --clients 1000 \
    --commit 8f168e8e9a11a68d5c9245b60beeb153904ccf62 --run-dir <dir>/iocp --run-id 8f168e8e0001c0c1
```

The runner was started at 07:52. The same head was built on every host. The
exclusive lock was granted at 08:14:15, and the client-host lock was taken
in the same second. Load1 per logical core count: at the start 10.24/64,
2.29/16 and 3.12/16; at 60 s 3.84/64, 0.31/16 and 0.63/16; at the end
8.32/64, 0.37/16 and 0.63/16. The run was admitted, and no bound was
crossed.

The server was granted `iocp` on port 40000 (`split_ready ... transport=iocp
transport_pin=iocp`). Every one of the 1000 clients went from `requesting
cookie` back to `disconnected` when its 10-second connect window ran out.
The only connections in the shard logs are the four local start-up probes,
one per shard, inside each client host.
Every shard receipt says `connected=0`, with the shards on `gso`. The server
receipt says `connected=0 startup_complete=0` at the end of its 60-second
connect window. Every server packet counter is 0, so not one client packet
reached the server process. Shard tail, addresses redacted:

```text
info: client changed state from 'requesting cookie' to 'disconnected'
info: message connection 0 ended: reason 0
split_receipt: ... role=client-shard ... first=0 count=250 ... connected=0 startup_complete=0 ...
split_proof: role=client-shard ... transport=gso transport_pin=none ...
```

Cause, read with `Get-NetFirewallProfile` and `Get-NetFirewallRule` in a
shared slot. The firewall is on for the Domain, Private and Public profiles,
and no profile has an inbound default. The one inbound allow rule that names
`load_split.exe` is an application rule for an older checkout path. The
runner's build lives at a path per commit, and no rule names that path or
allows the server's UDP port. So the host drops the clients' cookie
requests. docs/WINDOWS.md ("The firewall") says a headless server host needs
an inbound UDP rule for the server's port, and that this rule is the host's
provisioning. This is a host fact, not a library result. The cell is
recorded as a failure and was not rerun.

### rio cell

```text
scripts/lan-proof --inventory <inventory> --cell rio --duration 600 --clients 1000 \
    --commit 8f168e8e9a11a68d5c9245b60beeb153904ccf62 --run-dir <dir>/rio --run-id 8f168e8e0002f10a
```

The runner was started at 08:47, and the server's exclusive lock was
granted at 09:28:05. By then the client-host lock was held by other work, so
the run was stopped before any shard started. The server was stopped with
it. The rio cell was not made. With the firewall as it is, it would fail the
same way as iocp.

### Merger fix (scripts/lan-proof)

The runner told the merger `--client-transport portable`. Since the client
transport ranking landed, Linux shards come up on `gso`, so the merger
refused every cell with `client transport differs from requested transport`.
The runner now reads the transport in each shard's `split_receipt`. It
refuses a cell whose shards disagree, or that name `portable` or `none`, and
passes the shards' common transport to the merger. Checks of the runner's
own merge lines on collected logs:

- A complete 1000-client, 600 s cell from a macOS server, with shards on
  `gso`. The merger had refused it with `client transport differs from
  requested transport`. With the fix the runner prints
  `client transport gso on every shard`, and the merger gives
  `split_proof_cell: verdict=PASS ... streams=26000 messages_sent=24000000
  messages_delivered=24000000`.
- The same logs with one shard receipt edited to `transport=portable`. The
  runner refuses: `shard2 names no native client transport: 'portable'`.
- `scripts/lan-proof-selftest` prints `lan-proof-selftest: ok`.

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

1. The server host needs an inbound firewall rule that allows UDP on the
   proof port (40000) from the LAN. This is the host's provisioning
   (docs/WINDOWS.md, "The firewall"), and the library never changes it.
   Without the rule, every Windows cell fails as iocp did.
2. Then both cells at 600 s with 1000 clients. Run each one while the client
   hosts are free, and the server's exclusive lock can be granted within
   the runner's 900 s lock wait.

The pin fix is now shown by a run (the pinned start above). The
client transport given to the merger is fixed and checked on collected logs.

The known history of raw UDP drops on the old Windows 1 GbE path is not
exercised here. The server's test adapter negotiates 10 GbE, and the client
links negotiate 1 GbE.
