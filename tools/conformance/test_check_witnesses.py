#!/usr/bin/env python3

#   RocketNet. Copyright 2026 Más Bandwidth LLC.
#   Licensed under the GNU Affero General Public License v3.0

"""Tests for tools/conformance/check_witnesses.py.

Each test builds a small fixture tree in a temporary directory -- a
standard/TRANSPORT.md, a standard/MESSAGES.md, a standard/RATIONALE.md, a
proofs/tla/ model, a code file and a test file -- and drives the checker against
it. The checker is a pure function of its root, so the tests need no
network, no time and no socket, and each stays inside its own temporary
directory.

Run:  python3 tools/conformance/test_check_witnesses.py
"""

import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_witnesses  # noqa: E402


TRANSPORT = """# fixture transport

<a id="t1"></a>
## t1. Fixture

a receiver MUST drop a packet below its floor.
a sender MUST NOT exceed its budget.
"""

MESSAGES = """# fixture messages
"""

RATIONALE = """# fixture rationale

<a id="t1"></a>
## t1. Fixture

a receiver must drop a packet below its floor.
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

HEADER = "section\trule\ttla_invariant\tcode_function\ttest\ttier\n"


class CheckerFixture(unittest.TestCase):
    def build(self, rows):
        root = tempfile.mkdtemp(prefix="witnesses-")
        os.makedirs(os.path.join(root, "docs"))
        os.makedirs(os.path.join(root, "standard"))
        os.makedirs(os.path.join(root, "proofs", "tla"))
        os.makedirs(os.path.join(root, "sdk"))
        with open(os.path.join(root, "standard", "TRANSPORT.md"), "w") as handle:
            handle.write(TRANSPORT)
        with open(os.path.join(root, "standard", "MESSAGES.md"), "w") as handle:
            handle.write(MESSAGES)
        with open(os.path.join(root, "standard", "RATIONALE.md"), "w") as handle:
            handle.write(RATIONALE)
        with open(os.path.join(root, "proofs", "tla", "Fixture.tla"), "w") as handle:
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
            ["t1", "a receiver MUST drop a packet below its floor",
             "BelowFloorIsRefused", "sdk/code.cpp:fixture_below_floor_check",
             "sdk/tests.cpp:test_below_floor", "unit"],
            ["t1", "a sender MUST NOT exceed its budget",
             "SenderWithinBudget", "sdk/code.cpp:fixture_budget_check",
             "sdk/tests.cpp:test_budget", "unit"],
        ])
        code, report = check_witnesses.run(root)
        self.assertEqual(code, 0, report)
        self.assertIn("every named anchor resolves", report)

    def test_gap_is_counted_not_failed(self):
        root = self.build([
            ["t1", "a receiver MUST drop a packet below its floor",
             "-", "-", "-", "none"],
        ])
        code, report = check_witnesses.run(root)
        self.assertEqual(code, 0, report)
        self.assertIn("GAPS (tier none): 1", report)

    def test_review_is_explicit_and_requires_a_disposition(self):
        root = self.build([
            ["t1", "a receiver MUST drop a packet below its floor", "-",
             "sdk/code.cpp:fixture_below_floor_check", "docs/audits/review.md:floor", "review"],
        ])
        os.makedirs(os.path.join(root, "docs", "audits"))
        path = os.path.join(root, "docs", "audits", "review.md")
        with open(path, "w") as handle:
            handle.write('<a id="floor"></a>\nSource review with limits.\n')
        code, report = check_witnesses.run(root)
        self.assertEqual(code, 0, report)
        self.assertIn("SOURCE REVIEWS (not executable proof): 1", report)
        with open(path, "w") as handle:
            handle.write("The disposition was removed.\n")
        code, report = check_witnesses.run(root)
        self.assertEqual(code, 1, report)
        self.assertIn("does not resolve", report)

    def test_review_cannot_silently_relabel_a_unit_test(self):
        root = self.build([
            ["t1", "a receiver MUST drop a packet below its floor", "-",
             "sdk/code.cpp:fixture_below_floor_check", "sdk/tests.cpp:test_below_floor", "review"],
        ])
        code, report = check_witnesses.run(root)
        self.assertEqual(code, 1, report)
        self.assertIn("named docs/audits Markdown disposition", report)

    def test_deferred_mode_requires_a_boundary_witness_and_is_counted(self):
        row = ["t1", "a receiver MUST drop a packet below its floor", "-",
               "sdk/code.cpp:fixture_below_floor_check", "sdk/tests.cpp:test_below_floor", "deferred"]
        code, report = check_witnesses.run(self.build([row]))
        self.assertEqual(code, 0, report)
        self.assertIn("DEFERRED MODES (not implemented): 1", report)
        row[4] = "-"
        code, report = check_witnesses.run(self.build([row]))
        self.assertEqual(code, 1, report)
        self.assertIn("names no test", report)

    def test_renamed_section_anchor_fails(self):
        root = self.build([
            ["t9", "a receiver MUST drop a packet below its floor",
             "-", "-", "-", "none"],
        ])
        code, report = check_witnesses.run(root)
        self.assertEqual(code, 1)
        self.assertIn("does not resolve", report)

    def test_stale_code_anchor_fails(self):
        root = self.build([
            ["t1", "a receiver MUST drop a packet below its floor",
             "-", "sdk/code.cpp:fixture_renamed_away", "-", "unit"],
        ])
        code, report = check_witnesses.run(root)
        self.assertEqual(code, 1)
        self.assertIn("does not resolve", report)

    def test_stale_rule_text_fails(self):
        root = self.build([
            ["t1", "a receiver MUST drop a packet below its ceiling",
             "-", "-", "-", "none"],
        ])
        code, report = check_witnesses.run(root)
        self.assertEqual(code, 1)
        self.assertIn("no longer carries the rule text", report)

    def test_rule_text_kept_in_rationale_passes(self):
        root = self.build([
            ["t1", "a receiver must drop a packet below its floor",
             "-", "-", "-", "none"],
        ])
        code, report = check_witnesses.run(root)
        self.assertEqual(code, 0, report)

    def test_stale_tla_invariant_fails(self):
        root = self.build([
            ["t1", "a receiver MUST drop a packet below its floor",
             "RemovedInvariant", "-", "-", "none"],
        ])
        code, report = check_witnesses.run(root)
        self.assertEqual(code, 1)
        self.assertIn("is in no proofs/tla/*.tla", report)

    def test_stale_test_anchor_fails(self):
        root = self.build([
            ["t1", "a receiver MUST drop a packet below its floor",
             "-", "-", "sdk/tests.cpp:test_deleted", "unit"],
        ])
        code, report = check_witnesses.run(root)
        self.assertEqual(code, 1)
        self.assertIn("does not resolve", report)


if __name__ == "__main__":
    unittest.main(verbosity=2)
