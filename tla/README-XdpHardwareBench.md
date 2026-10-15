# XdpHardwareBench: the dedicated XDP machine's lease

`XdpHardwareBench.tla` models the dedicated XDP test machine that
`bench/xdp_hardware/` runs on. Runs take it one at a time: hold it, verify
it, attach, measure, then restore it. Restore is unconditional, and a run
can crash at any point. The scripts implement the model:
`bench/xdp_hardware/hold.sh` and `bench/xdp_hardware/restore.sh` name the
action each step implements. The library's own state machines are not part
of this model.

## States

`MachineState` derives the machine's state from the variables:

| State | Meaning |
|---|---|
| Free | no holder, nothing attached, the baseline intact |
| Held | a run holds the lock and has not verified yet |
| Verified | the holder's `rocketnet-provision verify` passed |
| Attached | the holder's receiver attached its program and bound its sockets |
| Running | the holder's sender and receiver are measuring |
| Restoring | the holder, or a recovering hold, is putting the machine back |
| Dirty | no holder, but a crashed run left state behind |

## Actions and their implementation

| Action | Implementation |
|---|---|
| `Hold(r)` | `flock -n` on `/run/rocketnet-xdp-hw/lock`, then write the run's pid, start time and boot id into the lock file |
| `Refused(r)` | `flock -n` fails: exit 75 naming the holder, changing nothing |
| `Verify(r)` | `run.sh`'s `preflight_verify`. Verify checks R3 (no XDP program attached), so in the model it can pass only with nothing attached and the baseline intact |
| `Attach(r)` | the receiver's `rocketnet_server_transport_create` on the xdp driver |
| `Run(r)`, `Finish(r)` | the rows, then the end of `run.sh` (pass or fail) |
| `Restore(r)` | `restore.sh` from the `EXIT` trap, which `INT` and `TERM` also reach. The lock file is emptied, and the lock released, only after it passes |
| `Crash(r)` | the run dies without its trap. The kernel drops the `flock` with the process. The lock file still names the run (`MarkCrash`) |
| `Recover`, `RecoverRestore` | the next `hold.sh`, or `hold.sh --recover`, finds a run named in a free lock and runs `restore.sh --recover` before anything else |
| `Quiet` | every run has ended and the machine is Free: stuttering, not deadlock |

## Properties

- `OneRunAtATime`: at most one run (or one recovery) holds the machine.
- `AttachOnlyAfterVerify`: no run has attached unless its verify passed.
- `FreeMeansRestored`: when the machine is Free, nothing is attached and
  the baseline is intact.
- `EveryHoldEndsFree` (liveness): every hold ends with the machine Free.
  The fairness assumption is weak fairness on restore and recovery, and on
  each run's own steps. Hold and Crash are never forced.

## Abstractions

- Restore always succeeds in the model. In the scripts, restore checks
  itself: it diffs the host snapshot against the one taken at hold and
  runs verify again. A failed restore leaves `restore-failed`, and the
  model has no matching transition. Every later hold refuses until a
  person clears it, so a failed restore is held for a person and never
  handed to the next run.
- An attach is modelled as one change to the baseline. The snapshot
  stands for the many host facts it covers (RUNBOOK.md, "Restore").
- Two runs are enough to cover contention. Every crash point is open: a
  run may crash in any holding state, including Restoring.

## Cases (`tla/CASES.tsv`, group `xdphardwarebench`)

| Case | Constants | Expected |
|---|---|---|
| `MCXdpHardwareBench.cfg` | `UseLock = TRUE`, `MarkCrash = TRUE` | pass: every invariant and `EveryHoldEndsFree` |
| `MCXdpHardwareBenchBrokenNoRestore.cfg` | `MarkCrash = FALSE` | fails `FreeMeansRestored` |
| `MCXdpHardwareBenchBrokenTwoRuns.cfg` | `UseLock = FALSE` | fails `OneRunAtATime` |

These are the measured results in `tla/RUNS.tsv`. They were run with the
pinned tlacheck and TLC JAR that `tla/README.md` names, on a Linux bench, one
shard per case:

- `MCXdpHardwareBench.cfg`: exit 0, 273 states generated, 169 distinct, no
  error. The liveness property was checked.
- `MCXdpHardwareBenchBrokenNoRestore.cfg`: exit 12,
  `Invariant FreeMeansRestored is violated`.
- `MCXdpHardwareBenchBrokenTwoRuns.cfg`: exit 12,
  `Invariant OneRunAtATime is violated`.

## The traces, replayed by hand

**BrokenNoRestore** (5 states). r1 holds (`lock = r1`), verifies
(`verified[r1]`), and attaches (`attachedBy = {r1}`, `baseline = FALSE`).
Then r1 crashes. The flock goes with the process (`lock = none`), and with
`MarkCrash = FALSE` nothing records the crash (`dirty = FALSE`). The
machine reads Free while r1's program is still attached.

In the scripts, the same point is a `kill -9` of `hold.sh` after the
receiver attached. What prevents the violation is the lock file, which
still names the dead run, because only a successful restore empties it.
The next `hold.sh` takes the flock, finds the file non-empty (`[ -s "$LOCK"
]`), and runs `restore.sh --recover` before anything else. Remove that
check, or empty the file before restore, and the scripts reproduce this
trace.

**BrokenTwoRuns** (3 states). r1 holds. With `UseLock = FALSE`, r2 holds
too, and both runs are in `held`. In the scripts, that is `hold.sh` without
`flock -n 9` and its refusal. The check that prevents it is the
`if ! flock -n 9` branch, which exits 75 naming the holder.

## Refreshing

As `tla/README.md` says:

```sh
for i in 0 1 2; do
  "$job/tlacheck" run --root . --jar "$jar" --java "$java" \
    --group xdphardwarebench --shards 3 --shard "$i" --workers 2 \
    --timeout 110s --dir "$job/xdphardwarebench-$i"
done
"$job/tlacheck" merge --root . --keep tla/RUNS.tsv \
  --out "$job/RUNS.tsv" "$job"/xdphardwarebench-*/RUNS.tsv
```
