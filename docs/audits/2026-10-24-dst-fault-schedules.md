# Deterministic simulation fault schedules, 2026-10-24

Audit of the deterministic simulation at `b0e281c4126c43f268b5129ee29539b416b17a9c` (`main`).
Scope read: `docs/DST.md`, `docs/CHANNELS.md`, `tests/dst.cpp`, `tests/dst_harness.h`,
`tests/dst_checks.h`, the `sdk/` files named below, and every audit already under `docs/audits/`.
Method: a Release build on Linux, then `./bin/dst --seeds 1-1000 --max-fails 1000` for each
schedule and for the control (`--faults all`, no `--schedule`); each distinct failure was replayed
from its first failing seed with `--verbose`, and the trace was read from the fault decisions and
the per-message sends and deliveries. This report changes no source, test, tool or workflow file.

| schedule | seeds | pass | fail | fired | trace |
|---|---:|---:|---:|---:|---|
| control | 1-1000 | 1000 | 0 | — | `cbc9714e690afbd6` |
| clock-back | 1-1000 | 989 | 11 | 1000 | not printed; seed 13 replay `d8df740043f82dc1` |
| seq-gap | 1-1000 | 1000 | 0 | 1000 | `652844a328436bf4` |
| seq-wrap-31 | 1-1000 | 1000 | 0 | 1000 | `3f0fd7132144bd04` |
| seq-wrap-32 | 1-1000 | 1000 | 0 | 1000 | `73ef08bb405ae3ee` |
| burst | 1-1000 | 999 | 1 | 984 | not printed; seed 448 replay `b4762428015746f4` |
| dup-storm | 1-1000 | 1000 | 0 | 1000 | `30c532c2ca82c2e8` |

`trace=` is printed only when every seed in the range passes, so the two failing rows give the
first failing seed's replay trace instead of a range trace.

## Findings

### 1. quality — `tests/dst_checks.h:989`

- Defect: The refusal table at `tests/dst_checks.h:989` explains replay, keep-alive, payload and fragment refusals but not `CONNECTION_REQUEST_COOKIE_EXPIRED`; the clock-back schedule rewinds one endpoint's protocol clock (`tests/dst_harness.h:1312`) while the server judges the cookie on the shared unix timestamp passed at `tests/dst_harness.h:513`, expires it at `cookie_expire <= current_timestamp` (`sdk/rocketnet_server_protocol.cpp:485`) after the three-second lifetime set at `tests/dst_harness.h:1465` and `sdk/rocketnet_server_protocol.cpp:498`.
- Failure or cost: 11 of 1000 clock-back seeds fail at step 4000 with `check=unexplained_refusal` and `what=server: CONNECTION_REQUEST_COOKIE_EXPIRED`. The first is seed 13, counter value 2. Replay: `./bin/dst --seeds 13-13 --shape full --schedule clock-back --verbose`; it reports `clock_back=1` and trace `d8df740043f82dc1`. There the schedule rewinds client 0 (schedule endpoint 1) by 2967 ms at step 708; the server issues a cookie at shared timestamp 1700000007 expiring at 1700000010 and receives the client's reply at shared timestamp 1700000010, so the refusal is caused by the schedule, not by a library defect.
- Smallest fix: When the clock-back schedule fired, treat `CONNECTION_REQUEST_COOKIE_EXPIRED` as explained in the refusal table, or draw a step-back shorter than the three-second lifetime. Not applied here.

### 2. quality — `tests/dst_checks.h:944`

- Defect: The end-of-run type-4 branch at `tests/dst_checks.h:944` judges reliable-sequenced liveness only from type-4 deliveries, so it cannot see that a newer type-1 sequenced message on the same channel has legally superseded the type-4 message; `ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED` guarantees only that the newest message on the channel arrives and may skip an older one (`sdk/rocketnet.h:460`), and the two sequenced types share the channel's ordering (`sdk/rocketnet_channel_sequenced.cpp:256`, `sdk/rocketnet_channel.cpp:327`).
- Failure or cost: 1 of 1000 burst seeds fails, seed 448, at step 4000, `check=liveness_delivered`, `what=stream=1292 quiet_start=120 newest=120 sent_count=35`. The burst fired on 984 seeds. Replay: `./bin/dst --seeds 448-448 --shape full --schedule burst --verbose`; it reports `fired=1` and trace `b4762428015746f4`. Stream 1292 is session 17, channel 8, type 4. In that replay a 14819-byte type-4 block (application sequence 129, 13 fragments, numbers 969-981) is sent at step 1999; the first three fragments (970, 975, 976) arrive with channel sequencing 0 and are held as an in-turn block. A type-1 message with channel sequencing 1 is sent at step 2000; it arrives with the same ordering and sequencing 1 against the block's sequencing 0, so `sdk/rocketnet_channel_sequenced.cpp:256` releases the in-turn block as superseded and the type-1 message is delivered at step 2011. The remaining block fragments then arrive with the channel watermark at 2 and are rejected at `sdk/rocketnet_channel_sequenced.cpp:262`. That is the documented rule that a newer in-turn sequenced message supersedes an incomplete in-turn block (`docs/CHANNELS.md:52`). The harness sees only type-4 deliveries, so it prints `newest=120` and reports a false liveness failure; no library behaviour is wrong.
- Smallest fix: Record the newest sequenced send type per session and channel in `rocketnet_dst_check_sent`, and in the type-4 branch of `rocketnet_dst_check_end` skip the liveness requirement when type 1 is the last sequenced type sent on that channel (or record the largest send step of any type-1 or type-4 delivery per channel and accept the type-4 stream when it reaches that stream's last send step). Not applied here.

## Counts

- bug: 0
- risk: 0
- quality: 2 (findings 1, 2)
- total: 2

## Cite check

Each `file:line` above was read back from the tree with `git show HEAD:<file> | sed -n '<line>p'`:

- `tests/dst_checks.h:989` — `        const int explained =`
- `tests/dst_harness.h:1312` — `        s->clock_offset[s->endpoint] -= (double) s->amount * 0.001;`
- `tests/dst_harness.h:513` — `                                                                  w->unix_ts, time,`
- `sdk/rocketnet_server_protocol.cpp:485` — `            else if ( cookie_expire <= current_timestamp )`
- `tests/dst_harness.h:1465` — `    w->server.cookie_lifetime_seconds = 3;`
- `sdk/rocketnet_server_protocol.cpp:498` — `            uint64_t fresh_expire = current_timestamp + (uint64_t) server->cookie_lifetime_seconds;`
- `tests/dst_checks.h:944` — `                    if ( !any || newest <= s->quiet_start_seq )`
- `sdk/rocketnet.h:460` — `    /// newest message on the channel and can drop a stale one.`
- `sdk/rocketnet_channel_sequenced.cpp:256` — `            rn_sequenced_release_in_turn( channels, rx );`
- `sdk/rocketnet_channel.cpp:327` — `    const uint32_t sequence_gap = ( sequencing - receive->sequenced_watermark ) & ROCKETNET_MESSAGE_U24_MASK;`
- `sdk/rocketnet_channel_sequenced.cpp:262` — `    if ( sequence_gap >= ( 1u << 23 ) )`
- `docs/CHANNELS.md:52` — `in-turn message supersedes an in-turn block still collecting fragments, and`

## Already filed

None of these two failures appears in the other audits under `docs/audits/` at this base.
