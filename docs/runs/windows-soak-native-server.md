# Windows soak, native server transports

The iocp and rio server cells of `soak-windows` (`.github/workflows/nightly-soak.yml`)
each ran once for 1800 seconds on the fleet's Windows bench, at one head, as
same-host loopback runs (correctness only; no NIC is exercised). Both cells
FAILED `--assert-flat` on the handle count, and the verdict is HOLD.

- Date (`date -u`): Sun Oct 25 2026 UTC. iocp start 04:52:55, finish 05:22:55;
  rio start 05:27:56, finish 05:57:57.
- Source head for the build and both cells:
  `19b80d62608fe23a5448638f334c5d42095dfd95` (main at `ccd72256` plus the two cells).
- Machine: `hostname` = `<Windows bench>` (Windows 11, MSVC 18 Community);
  `uname -r` from the bench's bash = `6.18.33.2-microsoft-standard-WSL2`. The
  Windows build number was not captured.
- Build: `cmake -B build -A x64` then `cmake --build build --config Release --parallel 4`
  under a shared bench slot; exit 0, 0 MSVC warnings, last line
  `test.vcxproj -> ...\bin\Release\test.exe`.
- `./bin/Release/test.exe`: exit 0, `*** ALL TESTS PASSED ***`.

## Commands

Each cell held the bench's exclusive lock for its whole run. Both ends were
impaired with the values `scripts/soak-per-transport` uses, and the bench's
bash forwarded them to the Windows process through `WSLENV`:

```sh
export ROCKETNET_SERVER_TRANSPORT=<iocp|rio> ROCKETNET_CLIENT_TRANSPORT=portable
for end in SERVER CLIENT; do
  export "ROCKETNET_${end}_SIMULATED_LOSS_PERCENT=5" "ROCKETNET_${end}_SIMULATED_LATENCY_MS=30" \
         "ROCKETNET_${end}_SIMULATED_JITTER_MS=20" "ROCKETNET_${end}_SIMULATED_DUPLICATE_PERCENT=2"
done
export WSLENV=$(env | grep -oE '^ROCKETNET_[A-Z_]+' | paste -sd: -)
timeout 1920 ./bin/Release/soak.exe --seconds 1800 --assert-flat
```

## Cells

| cell | granted server | handles start/end/peak | threads | os_threads | private_bytes start/end/peak | library_bytes start/end | exit | result |
|---|---|---|---|---|---|---|---|---|
| iocp | iocp | 115 / 113 / 121 | 3 / 3 / 4 | 2 / 0 / 2 | 133611520 / 133591040 / 153202688 | 131279561 / 131279561 | 1 | FAIL |
| rio | rio | 106 / 104 / 112 | 2 / 2 / 3 | 2 / 0 / 2 | 146481152 / 146436096 / 166088704 | 125110653 / 125110653 | 1 | FAIL |

Memory guard printed: iocp `memory guard library_bytes`; rio
`memory guard library_bytes and private_bytes (server rio, client portable)`.

iocp log lines:

```
soak transport: server iocp
soak resources: handles start=115 end=113 peak=121
soak: --assert-flat FAILED: handles start=115 end=113
soak resources: threads start=3 end=3 peak=4
soak resources: private_bytes start=133611520 end=133591040 peak=153202688
soak resources: os_threads start=2 end=0 peak=2
soak resources: library_bytes start=131279561 end=131279561 peak=151151286
soak summary: sent 2168660
soak summary: received 2168483
soak summary: verified 2168483
soak summary: reconnects 59
soak summary: server_transport_io iocp
```

rio log lines:

```
soak transport: server rio
soak resources: handles start=106 end=104 peak=112
soak: --assert-flat FAILED: handles start=106 end=104
soak resources: threads start=2 end=2 peak=3
soak resources: private_bytes start=146481152 end=146436096 peak=166088704
soak resources: os_threads start=2 end=0 peak=2
soak resources: library_bytes start=125110653 end=125110653 peak=144982378
soak summary: sent 2175878
soak summary: received 2175699
soak summary: verified 2175699
soak summary: reconnects 59
soak summary: server_transport_io rio
```

## Reading

Threads, private bytes and library bytes are flat on both cells; every
received payload verified. The only failure is the handle count, and it
FALLS by 2 on both cells in the same run in which os_threads falls from 2
to 0. The flat rule (`tests/process_resources.h`) already lets os_threads
fall on Windows, since the OS retires its pool workers on its own timers,
but it holds the handle count to exact equality, so a fall in handles that
comes with those retired workers fails the run. This record does not change
that rule; whether a handle fall that matches an os_threads fall is allowed
is a change to `tests/process_resources.h`, outside this record.

## Verdict

HOLD. iocp: FAIL (handles 115 -> 113). rio: FAIL (handles 106 -> 104).
Not made: a PASS of either cell; the Windows build number of the bench.
