#!/usr/bin/env python3

#   RocketNet. Copyright 2026 Más Bandwidth LLC.
#   Licensed under the GNU Affero General Public License v3.0

"""Tests for tools/conformance/check_witnesses.py.

Each test builds a small fixture tree in a temporary directory -- a
a standard, a tla/ model, a code file and a test file -- and drives the
checker against it. The checker is a pure function of its root, so the tests
need no network, no time and no socket, and each stays inside its own
temporary directory.

Run:  python3 tools/conformance/test_check_witnesses.py
"""

import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_witnesses  # noqa: E402


STANDARD = """# fixture standard

line one carries no rule.
a receiver MUST drop a packet below its floor.
a sender MUST NOT exceed its budget.
"""

TLA = """---- MODULE Fixture ----
BelowFloorIsRefused == TRUE
SenderWithinBudget == TRUE
====
"""

CODE = """// fixture code
int fixture_below_floor_check(void) { return 0; }
int fixture_budget_check(void) { return 0; }
"""

TESTS = """// fixture tests
test_below_floor
test_budget
"""

HEADER = "standard_line\trule\ttla_invariant\tcode_function\ttest\ttier\n"


class CheckerFixture(unittest.TestCase):
    def build(self, rows):
        root = tempfile.mkdtemp(prefix="witnesses-")
        os.makedirs(os.path.join(root, "docs"))
        os.makedirs(os.path.join(root, "tla"))
        os.makedirs(os.path.join(root, "sdk"))
        with open(os.path.join(root, "STANDARD.md"), "w") as handle:
            handle.write(STANDARD)
        with open(os.path.join(root, "tla", "Fixture.tla"), "w") as handle:
            handle.write(TLA)
        with open(os.path.join(root, "sdk", "code.cpp"), "w") as handle:
            handle.write(CODE)
        with open(os.path.join(root, "sdk", "tests.cpp"), "w") as handle:
            handle.write(TESTS)
        with open(os.path.join(root, "docs", "WITNESSES.tsv"), "w") as handle:
            handle.write(HEADER)
            for row in rows:
                handle.write("\t".join(row) + "\n")
        return root

    def test_every_anchor_resolves(self):
        root = self.build([
            ["4", "a receiver MUST drop a packet below its floor",
             "BelowFloorIsRefused", "sdk/code.cpp:fixture_below_floor_check",
             "sdk/tests.cpp:test_below_floor", "unit"],
            ["5", "a sender MUST NOT exceed its budget",
             "SenderWithinBudget", "sdk/code.cpp:fixture_budget_check",
             "sdk/tests.cpp:test_budget", "unit"],
        ])
        code, report = check_witnesses.run(root)
        self.assertEqual(code, 0, report)
        self.assertIn("every named anchor resolves", report)

    def test_gap_is_counted_not_failed(self):
        root = self.build([
            ["4", "a receiver MUST drop a packet below its floor",
             "-", "-", "-", "none"],
        ])
        code, report = check_witnesses.run(root)
        self.assertEqual(code, 0, report)
        self.assertIn("GAPS (tier none): 1", report)

    def test_stale_code_anchor_fails(self):
        root = self.build([
            ["4", "a receiver MUST drop a packet below its floor",
             "-", "sdk/code.cpp:fixture_renamed_away", "-", "unit"],
        ])
        code, report = check_witnesses.run(root)
        self.assertEqual(code, 1)
        self.assertIn("does not resolve", report)

    def test_stale_standard_keyword_fails(self):
        root = self.build([
            ["4", "a receiver MUST drop a packet below its ceiling",
             "-", "-", "-", "none"],
        ])
        code, report = check_witnesses.run(root)
        self.assertEqual(code, 1)
        self.assertIn("no longer carries the rule keyword", report)

    def test_stale_tla_invariant_fails(self):
        root = self.build([
            ["4", "a receiver MUST drop a packet below its floor",
             "RemovedInvariant", "-", "-", "none"],
        ])
        code, report = check_witnesses.run(root)
        self.assertEqual(code, 1)
        self.assertIn("is in no tla/*.tla", report)

    def test_stale_test_anchor_fails(self):
        root = self.build([
            ["4", "a receiver MUST drop a packet below its floor",
             "-", "-", "sdk/tests.cpp:test_deleted", "unit"],
        ])
        code, report = check_witnesses.run(root)
        self.assertEqual(code, 1)
        self.assertIn("does not resolve", report)


if __name__ == "__main__":
    unittest.main(verbosity=2)
