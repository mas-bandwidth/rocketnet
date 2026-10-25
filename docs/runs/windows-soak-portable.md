# Windows portable transport soak

One 1800-second run of the soak-windows portable cell in
`.github/workflows/nightly-soak.yml`: the portable server with the portable
client, the link simulator at both ends set to 30 ms latency, 20 ms jitter,
5 percent loss and 2 percent duplication, under `soak.exe --assert-flat`.

## Run

- Date (UTC): 2026-10-25, exclusive Windows lock held from 11:53:06Z; 60 s
  smoke 11:53:06Z to 11:54:06Z, 1800 s cell 11:54:06Z to 12:24:07Z.
- Machine: the fleet's native Windows bench, Windows 11
  (`Microsoft Windows [Version 10.0.26200.9711]`), MSVC, driven from WSL2
  (`uname -r`: `6.18.33.2-microsoft-standard-WSL2`). Load average before the
  smoke 0.04, after the cell 0.12; no other job held the lock during the run.
- Binary: head `ba660c1b` built in Release with
  `cmake -B build -A x64 && cmake --build build --config Release --parallel 4`
  (exit 0); `./bin/Release/test.exe` printed `*** ALL TESTS PASSED ***`
  under a shared slot before the cell. The step text the run executed is the
  one at `c7617b4b`, which changes only the workflow file (the log is read
  through an LF copy), so the binary is the same at both heads.
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

  Run from WSL rather than the runner's Git Bash, the harness also exported
  `WSLENV` naming those ten variables, since a Windows program started from
  WSL sees only the names `WSLENV` lists. The first smoke without it was
  granted the default client transport (iocp) and a clean link, and the step
  failed it as it should.

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
| portable smoke | 60 | 0 | `SOAK cell=portable PASS server=portable client=portable seconds=60 handles=111/111/118 threads=3/3/4 private_bytes=108060672/108064768/127639552 library_bytes=105680906/105680906/125552687 link=impaired missing=1872/1894 duplicates_refused=1117/1081` |
| portable | 1800 | 0 | `SOAK cell=portable PASS server=portable client=portable seconds=1800 handles=111/109/118 threads=3/3/4 private_bytes=108032000/107978752/127619072 library_bytes=105680906/105680906/125552687 link=impaired missing=56259/56480 duplicates_refused=33849/33995` |

The 1800-second cell's summary and resource lines:

```text
soak link witness: client_to_server missing=56259 late=669268 repeated=0 duplicates_refused=33849
soak link witness: server_to_client missing=56480 late=669061 repeated=0 duplicates_refused=33995
soak resources: memory guard library_bytes
soak resources: handles start=111 end=109 peak=118
soak resources: threads start=3 end=3 peak=4
soak resources: private_bytes start=108032000 end=107978752 peak=127619072
soak resources: os_threads start=2 end=0 peak=2
soak resources: library_bytes start=105680906 end=105680906 peak=125552687
soak summary: verified 2149167
soak summary: client_to_server_missing 56259
soak summary: server_to_client_missing 56480
soak summary: duplicates_refused_at_server 33849
soak summary: duplicates_refused_at_client 33995
soak summary: server_transport_io portable
soak summary: client_transport_io portable
soak summary: ROCKETNET_CLIENT_STATE_DISCONNECTED 60
```

Handles and threads end at or below their post-warmup counts, library bytes
end where they started, and private bytes are printed, not asserted, on the
portable transport. Both directions show drops and refused duplicates, so the
impairment was applied and observed.

## Verdict

PASS: the portable server with the portable client held flat handles, threads
and library bytes for 1800 s under loss, jitter and duplication. The native
server cells (iocp and rio) are recorded separately.
