# TLA+ Model Checking in CI

## Overview

This document describes the CI check that re-runs committed TLA+ models and verifies their results against the expected outcomes recorded in `tla/RUNS.tsv`.

## What Runs in CI

The CI workflow (`.github/workflows/tla.yml`) runs on:
- Push to `main` branch
- Pull requests touching `tla/` or `sdk/`
- Nightly schedule (02:00 UTC)

For each row in `tla/RUNS.tsv` where:
- The `distinct` state count is at most **2,000,000**
- The referenced config file exists in `tla/`
- The `result` is not "unfinished" or "not finished"

The check:
1. Parses the config description to build a temporary TLC config file with the specified overrides
2. Runs TLC with a **10-minute timeout** using `java -cp $TLA2TOOLS_JAR tlc2.TLC -config <cfg> <module>`
3. Compares the TLC result with the expected `result` column:
   - `pass` → TLC must complete with no invariant/property violations
   - `fail as expected: <invariant> violated` → TLC must fail on that specific invariant
   - State counts (`distinct` and `states`) must match the recorded digits

## What Is Skipped and Why

The check prints a **SKIPPED** line, with its reason and never silently, for:
- a row whose `distinct` > 2,000,000 — too large for CI runtime budget
- a row whose `result` contains "not finished" or "unfinished" — the bench run did not complete
- a committed `.cfg` file with no `RUNS.tsv` row — an owed run, see
  "Config-Row Consistency" below

The large or incomplete rows remain bench-only. Their results are recorded in
`RUNS.tsv` for reference but are not re-verified in CI. An owed config is not a
failure; it is listed so the missing row is visible.

Example skipped rows (from current `RUNS.tsv`):
- MCClientConnection with 772,894 distinct states (Timeout 3, Deadline 6...)
- MCClientConnection with 28,369,060 distinct states (OthersMayJoin TRUE, MaxDelay 1...)
- MCClientConnection with 74,297,106 distinct states (C2SPattern "first"...)
- MCClientConnection with 18,268,052 distinct states (C2SPattern "alternate"...)
- MCClientConnection with 24,756,464 distinct states (C2SPattern "burst"...)
- MCClientConnection with 75,959,281 distinct states (C2SPattern "arbitrary"...)
- MCClientConnection with 7,054,770 distinct states (MaxDelay 1...)
- MCClientConnection with 5,051,198 distinct states (C2SPattern "first"...)
- Rows marked "not finished (stopped at the card's deadline)"

## Config-Row Consistency

The check enforces the correspondence between the tree and the table:
- Every row in `RUNS.tsv` must reference an existing `.cfg` file. A row that
  names a config which is not committed is a dangling record, and the check
  **fails**.
- Every `.cfg` file in `tla/` that no row references is an **owed run** and is
  shown in the SKIPPED list, never silently: its model landed in parallel and
  its rows are recorded as owed in its `tla/README-*.md` before they are copied
  into `RUNS.tsv`. The check **does not fail** on these, so a tree whose models
  are ahead of its table still gets a green gate; the listed config is the
  prompt to land the owed row.

## How a Bench Result Gets Into RUNS.tsv

1. **Run the model locally** on a bench machine with TLC (using the full state space, no CI timeout).
2. **Record the exact output**: total states generated, distinct states found, and whether invariants/properties passed or failed (with the specific invariant name if failed).
3. **Add a row to `tla/RUNS.tsv`** with columns:
   - `module`: The TLA+ module name (e.g., `MCLink`, `MCClientConnection`)
   - `config`: Description of the config file and overrides (see format below)
   - `states`: Total states generated (from TLC output)
   - `distinct`: Distinct states found (from TLC output)
   - `result`: `pass` or `fail as expected: <InvariantName> violated` (or with parenthetical note like `(F4): Invariant X is violated`)
   - `bench`: Identifier for the bench machine (e.g., `vision`)
   - `date`: ISO date of the run (YYYY-MM-DD)
4. **Commit the updated `RUNS.tsv`** — the CI will verify it on next run.

### Config Description Format

The `config` column describes how to derive the run config from a base `.cfg` file:

```
<base.cfg> (Pattern "arbitrary", Spec, ResentIsDelivered)
<base.cfg> with Pattern "none", Spec, ResentIsDelivered
<base.cfg> with KindLoss "none"; checks Connects, ServerNotStranded
```

- Base file name (e.g., `MCLink.cfg`)
- Optional overrides in parentheses `(...)` or after `with`
- Overrides are comma-separated: `Key "value"` for strings, `Key value` for unquoted, `SpecWireFair` for specification, `PropertyName` for properties
- Optional `; checks <invariant>, <property>` suffix documents which invariants/properties are checked (informational only)

The parser applies these overrides to the base config file before running TLC.

## TLC Version

CI uses **TLA+ Tools v1.7.4** (`tla2tools.jar` SHA256: `936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88`), downloaded from the official GitHub release.

## Local Development

To run the check locally:

```bash
# Download tla2tools.jar v1.7.4
curl -L https://github.com/tlaplus/tlaplus/releases/download/v1.7.4/tla2tools.jar -o tla2tools.jar

# Run the check
export TLA2TOOLS_JAR=$(pwd)/tla2tools.jar
python3 tools/check_tla_runs.py
```

To run the unit tests (no Java/TLC required):

```bash
python3 tools/test_check_tla_runs.py
```
