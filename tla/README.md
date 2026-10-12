# tla: the rocketnet models and their tlacheck records

The TLA+ modules here specify the shared lossy UDP path (`Link.tla`) and the
client's connection state machine (`ClientConnection.tla`), each with a small
instance that proves it (`MCLink.tla` over `Link.tla`, `MCClientConnection.tla`
over `ClientConnection.tla`). The module documents are
`README-Link.md` and `README-ClientConnection.md`.

## The files

- `Link.tla`, `ClientConnection.tla` — the two modelled layers.
- `MCLink.tla`, `MCLink.cfg` — the link instance; the other `MCLink*.cfg`
  files are the same instance with the `SPECIFICATION`, `Pattern` and
  `PROPERTIES` lines changed as `README-Link.md` lists.
- `MCClientConnection.tla`, `MCClientConnection.cfg` — the connection
  instance; the other `MCClientConnection*.cfg` files are that file with the
  lines `README-ClientConnection.md` lists.
- `CASES.tsv` — the case plan: every `MC*.cfg` file exactly once, with the
  module it instantiates, the result it must reach (`pass`, or the invariant,
  action or temporal property a reversed witness or a finding breaks), its
  deadlock policy, its group (`link` or `clientconnection`), its gate, and its
  debt. The three cases that ran past the old card's deadline without a
  violation are declared debt (`gate bench`) and are never dropped.
- `RUNS.tsv` — the measured records, in tlacheck's nineteen-column layout.
  One record per case; it is produced by `tlacheck run` and `tlacheck merge`,
  never by hand (a record's `input_sha256` fingerprints the module files, the
  case's own `CASES.tsv` row and tlacheck's runner).
- `RUNS-HISTORY.tsv` — the pre-tlacheck records in the old seven-column
  layout, kept whole for reference.

## The runner

The records are produced and checked by `tlacheck`, the runner and record tool
in `mas-bandwidth/nova-tools` (`tools/tlacheck`, over `internal/tlc`). Build it
from a nova-tools checkout and run it on a Linux bench that has java and a
`tla2tools.jar` (`--jar`), never on a working machine:

```sh
go build -o /tmp/tlacheck ./tools/tlacheck
/tmp/tlacheck groups --root .            # the required groups, as JSON
/tmp/tlacheck groups --root . --stale    # the groups whose records are missing or stale
/tmp/tlacheck inputs --root . --case MCLink.cfg
```

## Measuring the records

`tlacheck run` measures the cases of one group into a clean directory and
writes that run's `RUNS.tsv`; `tlacheck merge` joins the group runs, in plan
order, into the committed `tla/RUNS.tsv`. The two groups here are `link` and
`clientconnection`:

```sh
runs=$(mktemp -d)
for g in link clientconnection; do
  /tmp/tlacheck run --root . --jar /path/to/tla2tools.jar --dir "$runs/$g" --group "$g"
done
/tmp/tlacheck merge --root . --out tla/RUNS.tsv "$runs"/*/RUNS.tsv
```

A group that ends over its 110 s budget exits 1 and still writes its records:
the cases declared debt in `CASES.tsv` are recorded as the failed measurements
they are. A case that is not declared debt and fails is a defect in the model
or the plan.

## Refreshing the records after a model edit

A model edit stales only the records of the cases that read the edited file: a
change to a module stales every case whose module extends or instantiates it, a
change to a configuration or a case's own `CASES.tsv` row stales that case, and
a change to tlacheck's result files stales every case. The author refreshes
only what the edit staled, in the same change that made the edit (a model edit
commits its records in the same change):

```sh
go build -o /tmp/tlacheck ./tools/tlacheck
/tmp/tlacheck groups --root . --stale    # [] means nothing staled
```

Otherwise the output lists the groups to run. On a Linux bench with java, run
each stale group into a clean directory of its own and join the runs onto the
committed records with `--keep` (which keeps a record only when its case was
not measured again and the record is still current):

```sh
runs=$(mktemp -d)
stale=$(/tmp/tlacheck groups --root . --stale | tr -d '[]"' | tr ',' ' ')
for g in $stale; do
  /tmp/tlacheck run --root . --jar /path/to/tla2tools.jar --dir "$runs/$g" --group "$g"
done
test -z "$stale" || /tmp/tlacheck merge --root . --keep tla/RUNS.tsv --out tla/RUNS.tsv "$runs"/*/RUNS.tsv
```

Commit `tla/RUNS.tsv` in the same change. To measure every case instead (a new
jar, or a runner change that stales everything), run every group and merge
without `--keep`.
