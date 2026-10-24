# Deterministic simulation fault schedules, 2026-10-24

Audit of the deterministic simulation at `bb180168fbf1ceb60e978157232f9403597ded1e`.
Scope read: `docs/DST.md`, `tests/dst.cpp`, `tests/dst_harness.h`, `tests/dst_checks.h`, and the audits already under `docs/audits/`.
No source, test, tool, or workflow file was changed. Each schedule and the control ran seeds 1-1000
(`./bin/dst --seeds 1-1000 --max-fails 1000`), the control as `--faults all` with no `--schedule`,
the others as `--schedule <name>`. A Release build and `./bin/test` passed at this sha before the schedules.
Schedules ran two at a time, and only while the 1-minute load was under half the core count;
a pair whose 1-minute load reached the core count was not kept. The kept runs stayed under it
(peak 1-minute sample 5.73 on the burst pair, 7.23 on dup-storm, 6.70 on the first four schedules).
The failing summary line does not print a range trace; `trace=` is printed only when every seed passes.
The replay of a failing seed prints one summary line (`--verbose`), not a per-packet trace.

| schedule | seeds | pass | fail | fired | trace |
|---|---:|---:|---:|---:|---|
| control | 1-1000 | 1000 | 0 | — | `cbc9714e690afbd6` |
| clock-back | 1-1000 | 989 | 11 | 1000 | not printed; seed 13 replay `d8df740043f82dc1` |
| seq-gap | 1-1000 | 1000 | 0 | 1000 | `652844a328436bf4` |
| seq-wrap-31 | 1-1000 | 1000 | 0 | 1000 | `3f0fd7132144bd04` |
| seq-wrap-32 | 1-1000 | 1000 | 0 | 1000 | `73ef08bb405ae3ee` |
| burst | 1-1000 | 999 | 1 | 984 | not printed; seed 448 replay `b4762428015746f4` |
| dup-storm | 1-1000 | 1000 | 0 | 1000 | `30c532c2ca82c2e8` |

seq-wrap-32 and burst were run a second time after the first pair's 1-minute load had climbed
well above what two schedules account for. The second run, started at 1-minute load 4.36 and
ended at 3.54 with peak sample 5.73, reproduced both rows, including the same burst seed and the
same seq-wrap-32 trace. Those are the rows above.

## Findings

### 1. quality — `tests/dst_checks.h:989`

- Defect: `rocketnet_dst_check_unexplained_refusal` does not treat `CONNECTION_REQUEST_COOKIE_EXPIRED` as explained by the clock-back schedule, whose step-back is 10 ms to 5 s while the connect cookie lifetime is 3 seconds (`sdk/rocketnet_server_transport.cpp:8`, stored at line 1829) and the expiry test is `cookie_expire <= current_timestamp` (`sdk/rocketnet_server_protocol.cpp:485`).
- Failure or cost: 11 of 1000 clock-back seeds fail at step 4000 with `check=unexplained_refusal` and `what=server: CONNECTION_REQUEST_COOKIE_EXPIRED`. The first is seed 13, counter value 2. Replay: `./bin/dst --seeds 13-13 --shape full --schedule clock-back`. That replay reports `clock_back=1`. The schedule rewinds one endpoint's protocol clock (`tests/dst_harness.h:1312`) while the cookie is judged on the shared unix clock (`tests/dst_harness.h:1604`), so a step-back longer than the remaining lifetime expires a cookie the refusal table does not expect.
- Smallest fix: When the clock-back schedule fired, count `CONNECTION_REQUEST_COOKIE_EXPIRED` as explained, or draw the step-back shorter than the 3-second lifetime. Not applied here.

### 2. quality — `tests/dst_checks.h:944`

- Defect: The end-of-run reliable-sequenced check fails a seed when the newest delivered sequence is still the sequence recorded at the close of the fault window, even though the burst's own batch was released.
- Failure or cost: 1 of 1000 burst seeds fails, seed 448, at step 4000, `check=liveness_delivered`, `what=stream=1292 quiet_start=120 newest=120 sent_count=35`. The burst fired on 984 seeds. Replay: `./bin/dst --seeds 448-448 --shape full --schedule burst`. That replay reports `burst_sent=256 burst_received=312` and the same what-line. Stream 1292 is session 17, channel 8, type 4 (`(17 * 9 + 8) * 8 + 4`), which is server-to-client `RELIABLE_SEQUENCED` on the mixed channel of client 1. The replay summary does not name an sdk line, so this pass does not attribute the miss to a library statement.
- Smallest fix: Not proposed as a protocol change. A replay that prints each delivery on that stream would separate a sender that stopped from a receiver that dropped the quiet-window sequences.

## Counts

- bug: 0
- risk: 0
- quality: 2 (findings 1, 2)
- total: 2

## Already filed

None of these two failures appear in the audits under `docs/audits/` at this sha.
