# Windows portable transport soak

One 1800-second run of the soak-windows portable cell in
`.github/workflows/nightly-soak.yml`: the portable server with the portable
client, the link simulator at both ends set to 30 ms latency, 20 ms jitter,
5 percent loss and 2 percent duplication, under `soak.exe --assert-flat`.

## Run

- Date (UTC): 2026-10-25. One exclusive Windows lock held from 12:49:33Z for
  both runs: 60 s smoke 12:49:33Z to 12:50:34Z, 1800 s cell 12:50:34Z to
  13:20:34Z.
- Machine: windows-server-1, the fleet's native Windows bench, Windows 11
  (`Microsoft Windows [Version 10.0.26200.9711]`), MSVC, driven from WSL2
  (`uname -r`: `6.18.33.2-microsoft-standard-WSL2`). Load average 0.17 before
  the smoke and 0.29 after the cell; no other job held the lock.
- Head: `74b8bb7f` (this branch merged with main), built in Release under an
  exclusive hold from 12:42:19Z with
  `cmake -B build -A x64 && cmake --build build --config Release --parallel 4`
  (exit 0, no MSVC warning lines). Both runs used that build. Its
  `./bin/Release/test.exe` stopped at
  `test_light_profile_server_1000_required_memory`
  (`light server lifecycle bytes: actual=1591206044 expected=1523705813`).
  That red comes from main: at this head the branch differs from main only in
  the workflow file and this record.
- Command: the workflow step's `run:` block with `seconds` set to 60 (smoke)
  and then 1800, which runs

```text
env ROCKETNET_SERVER_TRANSPORT=portable ROCKETNET_CLIENT_TRANSPORT=portable \
    ROCKETNET_DEV_SERVER_PACKET_LOSS_PERCENT=5 ROCKETNET_DEV_CLIENT_PACKET_LOSS_PERCENT=5 \
    ROCKETNET_DEV_SERVER_LATENCY_MS=30 ROCKETNET_DEV_CLIENT_LATENCY_MS=30 \
    ROCKETNET_DEV_SERVER_JITTER_MS=20 ROCKETNET_DEV_CLIENT_JITTER_MS=20 \
    ROCKETNET_DEV_SERVER_DUPLICATE_PERCENT=2 ROCKETNET_DEV_CLIENT_DUPLICATE_PERCENT=2 \
    ./bin/Release/soak.exe --seconds 1800 --assert-flat
```

  The harness ran from WSL, not the runner's Git Bash, so it also exported
  `WSLENV` with those ten variables. A Windows program started from WSL sees
  only the names `WSLENV` lists.

## Applied settings

```text
soak link simulator: server latency_ms=30 jitter_ms=20 loss_percent=5 duplicate_percent=2
soak link simulator: client latency_ms=30 jitter_ms=20 loss_percent=5 duplicate_percent=2
soak transport: server portable
soak transport: client portable
```

## Results

| cell | seconds | exit | verdict line |
| --- | --- | --- | --- |
| portable smoke | 60 | 0 | `SOAK cell=portable PASS server=portable client=portable seconds=60 handles=117/117/124 threads=3/3/4 private_bytes=108072960/108072960/127655936 library_bytes=105681137/105681137/125552918 link=impaired missing=1853/1882 duplicates_refused=1144/1060` |
| portable | 1800 | 0 | `SOAK cell=portable PASS server=portable client=portable seconds=1800 handles=111/109/118 threads=3/3/4 private_bytes=107819008/107847680/128389120 library_bytes=105681137/105681137/125552918 link=impaired missing=55468/56130 duplicates_refused=33369/34212` |

The 1800-second cell's summary and resource lines:

```text
soak link witness: client_to_server missing=55468 late=662475 repeated=0 duplicates_refused=33369
soak link witness: server_to_client missing=56130 late=661654 repeated=0 duplicates_refused=34212
soak resources: memory guard library_bytes
soak resources: handles start=111 end=109 peak=118
soak resources: threads start=3 end=3 peak=4
soak resources: private_bytes start=107819008 end=107847680 peak=128389120
soak resources: os_threads start=2 end=0 peak=2
soak resources: library_bytes start=105681137 end=105681137 peak=125552918
soak summary: verified 2130090
soak summary: client_to_server_missing 55468
soak summary: server_to_client_missing 56130
soak summary: duplicates_refused_at_server 33369
soak summary: duplicates_refused_at_client 34212
soak summary: server_transport_io portable
soak summary: client_transport_io portable
```

Handles and threads end at or below their post-warmup counts. Library bytes
end where they started. On the portable transport, private bytes are printed
but not asserted. Both directions show drops and refused duplicates, so the
impairment was applied and seen.

## Verdict

PASS: for 1800 s under loss, jitter and duplication, the portable server with
the portable client kept handles, threads and library bytes flat. The native
server cells (iocp and rio) have their own record,
docs/runs/windows-soak-native-server.md.
