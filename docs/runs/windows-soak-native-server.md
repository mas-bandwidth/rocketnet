# Windows soak, native server transports

The iocp and rio server cells of `soak-windows` (`.github/workflows/nightly-soak.yml`)
each ran once for 1800 seconds on the fleet's Windows bench, at one head, one
after the other inside one exclusive hold of the bench, as same-host loopback
runs (correctness only; no NIC is exercised). Both cells PASSED
`--assert-flat`, and the verdict is LAND.

- Date (`date -u`): Sun Oct 25 2026 UTC. iocp start 10:21:56, finish 10:51:56;
  rio start 10:51:56, finish 11:21:57.
- Source head for the build and both cells:
  `3f900fa1254d938e50150bcb3386a5a0bb857e1a` (main at `dda29fba` merged, plus
  the cells taking the `ROCKETNET_DEV_*` impairment names).
- Machine: `hostname` = `<Windows bench>`; `ver` = `Microsoft Windows [Version 10.0.26200.9711]`
  (Windows 11), MSVC 18 Community; `uname -r` from the bench's bash =
  `6.18.33.2-microsoft-standard-WSL2`.
- Build: `cmake -B build -A x64` then `cmake --build build --config Release --parallel 4`
  under a shared bench slot; exit 0, 0 MSVC warnings, last line
  `functional.vcxproj -> ...\bin\Release\functional.exe`.
- `./bin/Release/test.exe`: exit 0, `*** ALL TESTS PASSED ***`.

## Commands

Both ends were impaired with the values `scripts/soak-per-transport` uses, under
the names soak writes into the transport configs it creates from. The bench's
bash is WSL, so `WSLENV` forwarded them to the Windows process (the workflow's
Git Bash step needs no `WSLENV`):

```sh
export ROCKETNET_SERVER_TRANSPORT=<iocp|rio> ROCKETNET_CLIENT_TRANSPORT=portable
for end in SERVER CLIENT; do
  export "ROCKETNET_DEV_${end}_PACKET_LOSS_PERCENT=5" "ROCKETNET_DEV_${end}_LATENCY_MS=30" \
         "ROCKETNET_DEV_${end}_JITTER_MS=20" "ROCKETNET_DEV_${end}_DUPLICATE_PERCENT=2"
done
export WSLENV=$(env | grep -oE '^ROCKETNET_[A-Z_]+' | paste -sd: -)
timeout 1920 ./bin/Release/soak.exe --seconds 1800 --assert-flat
```

## Cells

| cell | granted server | applied link (server and client) | missing c->s / s->c | duplicates refused at server / client | handles start/end/peak | threads | os_threads | private_bytes start/end/peak | library_bytes start/end | verified | exit | result |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| iocp | iocp | latency_ms=30 jitter_ms=20 loss_percent=5 duplicate_percent=2 | 55898 / 56326 | 33158 / 33714 | 115 / 113 / 122 | 3 / 3 / 4 | 2 / 0 / 2 | 133632000 / 133582848 / 153231360 | 131279641 / 131279641 | 2139928 of 2139928 received | 0 | PASS |
| rio | rio | latency_ms=30 jitter_ms=20 loss_percent=5 duplicate_percent=2 | 55955 / 56469 | 33365 / 34044 | 106 / 104 / 113 | 2 / 2 / 3 | 2 / 0 / 2 | 146993152 / 146939904 / 166105088 | 143198772 / 143198772 | 2143993 of 2143993 received | 0 | PASS |

iocp log lines:

```
soak link simulator: server latency_ms=30 jitter_ms=20 loss_percent=5 duplicate_percent=2
soak transport: server iocp
soak transport: client portable
soak link simulator: client latency_ms=30 jitter_ms=20 loss_percent=5 duplicate_percent=2
soak link witness: client_to_server missing=55898 late=665116 repeated=0 duplicates_refused=33158
soak link witness: server_to_client missing=56326 late=665330 repeated=0 duplicates_refused=33714
soak resources: memory guard library_bytes
soak resources: handles start=115 end=113 peak=122
soak resources: threads start=3 end=3 peak=4
soak resources: private_bytes start=133632000 end=133582848 peak=153231360
soak resources: os_threads start=2 end=0 peak=2
soak resources: library_bytes start=131279641 end=131279641 peak=151151422
soak summary: sent 2255398
soak summary: received 2139928
soak summary: verified 2139928
soak summary: reconnects 59
soak summary: server_transport_io iocp
soak summary: client_transport_io portable
SOAK cell=iocp PASS server=iocp seconds=1800
```

rio log lines:

```
soak link simulator: server latency_ms=30 jitter_ms=20 loss_percent=5 duplicate_percent=2
soak transport: server rio
soak transport: client portable
soak link simulator: client latency_ms=30 jitter_ms=20 loss_percent=5 duplicate_percent=2
soak link witness: client_to_server missing=55955 late=666954 repeated=0 duplicates_refused=33365
soak link witness: server_to_client missing=56469 late=667394 repeated=0 duplicates_refused=34044
soak resources: memory guard library_bytes and private_bytes (server rio, client portable)
soak resources: handles start=106 end=104 peak=113
soak resources: threads start=2 end=2 peak=3
soak resources: private_bytes start=146993152 end=146939904 peak=166105088
soak resources: os_threads start=2 end=0 peak=2
soak resources: library_bytes start=143198772 end=143198772 peak=163070553
soak summary: sent 2259658
soak summary: received 2143993
soak summary: verified 2143993
soak summary: reconnects 59
soak summary: server_transport_io rio
soak summary: client_transport_io portable
SOAK cell=rio PASS server=rio seconds=1800
```

## Reading

The requested impairment was applied at both ends and left its trace: about
2.5% of the payloads in each direction never arrived and every planted
duplicate was refused as already received, so the flat lines below were taken
under loss, jitter and duplication, not on a clean link. Every received payload
verified. Threads, private bytes and library bytes are flat on both cells. The
handle count ends 2 below its start on both cells in the same run in which
os_threads falls from 2 to 0, as the OS retires its pool workers; the flat rule
(`tests/process_resources.h`) fails only on growth, and neither cell grew.

## Verdict

LAND. iocp: PASS. rio: PASS.
