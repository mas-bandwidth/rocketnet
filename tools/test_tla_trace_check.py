#!/usr/bin/env python3
"""Tests for the trace check's rows and shard table in tools/tla_trace_check.py: each way the rows, the
shard table or the workflows' shard lists can be wrong is shown red, and the repository's own is green.
No build, no TLC: the checks a run makes before any walk."""

import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tla_trace_check as ttc

REPO_ROOT = Path(__file__).resolve().parent.parent
WORKFLOWS = REPO_ROOT / ".github" / "workflows"
RULESET = REPO_ROOT / ".github" / "rulesets" / "main.json"

ROW = """# a row
{
    "layer": "%(layer)s",
    "modules": ["RNTraceExample.tla", "TraceRNTraceExample.tla"],
    "untraced": {},
    %(extra)s
    "commit": %(commit)s,
}
"""


class Rows(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())

    def tearDown(self):
        shutil.rmtree(self.dir)

    def layer(self, stem, layer=None, commit="{1: 0, 2: 0, 3: 1, 4: 1}", extra="", cpp=True, text=None):
        if cpp:
            (self.dir / f"tla_trace_{stem}.cpp").write_text("// a layer\n")
        (self.dir / f"tla_trace_{stem}.row").write_text(
            text if text is not None else ROW % {"layer": layer or stem, "commit": commit, "extra": extra})

    def failures(self, count=2):
        rows, stems, failed = ttc.load_rows(self.dir)
        return failed + ttc.table_failures(rows, stems, count=count)

    def red(self, text, count=2):
        failed = self.failures(count)
        self.assertTrue(any(text in f for f in failed), f"{text!r} not in {failed}")

    def test_good_rows_are_green(self):
        self.layer("a")
        self.layer("b", commit="{1: 1}")
        self.assertEqual(self.failures(), [])

    def test_layer_file_without_row(self):
        self.layer("a")
        (self.dir / "tla_trace_ghost.cpp").write_text("// a layer\n")
        self.red("tests/tla_trace_ghost.cpp has no row")

    def test_row_without_layer_file(self):
        self.layer("a")
        self.layer("ghost", cpp=False)
        self.red("tests/tla_trace_ghost.row is a row with no layer file")

    def test_two_rows_of_one_layer(self):
        self.layer("a")
        self.layer("b", layer="a")
        self.red("layer a: two rows")

    def test_key_twice_in_a_row(self):
        self.layer("a", extra='"steps": 200,\n    "steps": 300,')
        self.red("key 'steps' twice")

    def test_seed_in_two_shards(self):
        self.layer("a", commit="{1: 0, 2: 0, 2: 1, 3: 1}")
        self.red(f"SHARDS: a:2 is listed twice, in shards 0/{ttc.SHARD_COUNT} and 1/{ttc.SHARD_COUNT}")

    def test_seed_in_no_shard(self):
        self.layer("a", commit="{1: 0, 2: 0, 3: 1, 4: 2}")
        self.red("SHARDS: a:4 is listed in no shard")

    def test_layer_with_no_commit_seed(self):
        self.layer("a")
        self.layer("b", commit="{}")
        self.red("SHARDS: b is listed in no shard")

    def test_commit_seed_not_positive(self):
        self.layer("a", commit="{0: 0, 1: 1}")
        self.red("commit seed 0 is not a positive whole number")

    def test_empty_shard(self):
        self.layer("a", commit="{1: 0, 2: 0}")
        self.red("SHARDS: shard 1/2 holds no (layer, seed) pair")

    def test_unknown_key(self):
        self.layer("a", extra='"kind": "steps",')
        self.red("layer a: no row key kind")

    def test_missing_key(self):
        self.layer("a", text='{"layer": "a", "untraced": {}, "commit": {1: 0, 2: 1}}')
        self.red("layer a: the row has no modules")

    def test_steps_under_floor(self):
        self.layer("a", extra='"steps": 5,')
        self.red("layer a: steps 5 is under 100")

    def test_self_contained_elsewhere(self):
        self.layer("a", extra='"self_contained": "planted",')
        self.red("layer a: only the example is self-contained")

    def test_bad_name(self):
        self.layer("a", layer="A_b")
        self.red("layer A_b: a name is lower-case letters")

    def test_not_a_literal(self):
        self.layer("a")
        self.layer("b", text="{'layer': 'b', 'steps': 1 + 2}")
        self.red("tests/tla_trace_b.row: not a literal")

    def test_not_python(self):
        self.layer("a")
        self.layer("b", text="layer = b")
        self.red("tests/tla_trace_b.row: not a Python literal")

    def test_not_a_dict(self):
        self.layer("a")
        self.layer("b", text="['b']")
        self.red("tests/tla_trace_b.row: a row is one dict literal")

    def test_no_layer_key(self):
        self.layer("a")
        self.layer("b", text='{"modules": [], "untraced": {}, "commit": {1: 1}}')
        self.red('tests/tla_trace_b.row: no "layer"')


class Repository(unittest.TestCase):
    def test_the_repository_rows_are_green(self):
        rows, stems, failed = ttc.load_rows()
        self.assertEqual(failed + ttc.table_failures(rows, stems), [])
        self.assertEqual(ttc.workflow_failures(ttc.SHARD_COUNT), [])

    def test_every_pair_in_one_shard(self):
        rows, _, _ = ttc.load_rows()
        expanded, homes, failed = ttc.shard_table(rows, ttc.SHARD_COUNT)
        self.assertEqual(failed, [])
        pairs = [p for shard in expanded for p in shard]
        self.assertEqual(len(pairs), len(set(pairs)))
        self.assertEqual(set(pairs), {(n, s) for n in rows for s in ttc.commit_seeds(rows[n])})
        for n in rows:
            self.assertEqual(homes[n], rows[n]["commit"][ttc.commit_seeds(rows[n])[0]])


class Workflows(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        for name in ("ci.yml", "tlc.yml"):
            shutil.copy(WORKFLOWS / name, self.dir / name)
        shutil.copy(RULESET, self.dir / "main.json")
        self.paths = {"ci": self.dir / "ci.yml", "tlc_yml": self.dir / "tlc.yml", "ruleset": self.dir / "main.json"}

    def tearDown(self):
        shutil.rmtree(self.dir)

    def edit(self, name, old, new):
        path = self.dir / name
        text = path.read_text()
        self.assertIn(old, text)
        path.write_text(text.replace(old, new, 1))

    def test_ci_matrix_short(self):
        self.edit("ci.yml", ", '8/9']", "]")
        failed = ttc.workflow_failures(9, **self.paths)
        self.assertTrue(any(f.startswith("ci.yml job tla-trace: its matrix is not the 9 shards") for f in failed), failed)

    def test_nightly_matrix_short(self):
        self.edit("tlc.yml", ", '8/9']", "]")
        failed = ttc.workflow_failures(9, **self.paths)
        self.assertTrue(any(f.startswith("tlc.yml job trace: its matrix is not the 9 shards") for f in failed), failed)

    def test_ruleset_short(self):
        self.edit("main.json", '          {"context": "TLA+ trace check (fixed seeds, shard 8/9)", "integration_id": 15368},\n', "")
        failed = ttc.workflow_failures(9, **self.paths)
        self.assertTrue(any(f.startswith("main.json: requires the trace shards") for f in failed), failed)

    def test_write_shards_grows_every_list(self):
        self.assertNotEqual(ttc.workflow_failures(12, **self.paths), [])
        ttc.write_shards(12, **self.paths)
        self.assertEqual(ttc.workflow_failures(12, **self.paths), [])
        text = (self.dir / "main.json").read_text()
        self.assertIn('"TLA+ trace check (fixed seeds, shard 11/12)"', text)
        self.assertNotIn("/9)", text)
        # every other line of the files is untouched
        before = (WORKFLOWS / "ci.yml").read_text().splitlines()
        after = (self.dir / "ci.yml").read_text().splitlines()
        self.assertEqual(len(before), len(after))
        self.assertEqual(sum(1 for a, b in zip(before, after) if a != b), 1)
        ttc.write_shards(9, **self.paths)
        self.assertEqual((self.dir / "main.json").read_text(), RULESET.read_text())
        self.assertEqual((self.dir / "ci.yml").read_text(), (WORKFLOWS / "ci.yml").read_text())
        self.assertEqual((self.dir / "tlc.yml").read_text(), (WORKFLOWS / "tlc.yml").read_text())


if __name__ == "__main__":
    unittest.main()
