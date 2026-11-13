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
import shutil
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
        self.addCleanup(shutil.rmtree, root, ignore_errors=True)
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
        # the anchor goes and the word stays: prose naming the floor is no
        # disposition
        with open(path, "w") as handle:
            handle.write("The floor disposition was removed.\n")
        code, report = check_witnesses.run(root)
        self.assertEqual(code, 1, report)
        self.assertIn("review disposition 'docs/audits/review.md:floor' has no <a id=\"floor\"> anchor", report)
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

    def test_property_in_another_module_is_not_the_cited_one(self):
        # The base's replay row names a property that lives in a different
        # module than the one the row cites. A name that exists elsewhere
        # does not satisfy the citation.
        root = self.build([
            ["t1", "a receiver MUST drop a packet below its floor",
             "ReplayWindow.tla:ReceiverNeverOutsideWindow",
             "-", "-", "none"],
        ])
        tla = os.path.join(root, "proofs", "tla")
        with open(os.path.join(tla, "ReplayWindow.tla"), "w") as handle:
            handle.write("---- MODULE ReplayWindow ----\nReceivedIsRefused == TRUE\n====\n")
        with open(os.path.join(tla, "RNWindow.tla"), "w") as handle:
            handle.write("---- MODULE RNWindow ----\nReceiverNeverOutsideWindow == TRUE\n====\n")
        code, report = check_witnesses.run(root)
        self.assertEqual(code, 1, report)
        self.assertIn("ReceiverNeverOutsideWindow", report)
        self.assertIn("not defined in proofs/tla/ReplayWindow.tla", report)

    def test_property_defined_in_the_cited_module_passes(self):
        root = self.build([
            ["t1", "a receiver MUST drop a packet below its floor",
             "proofs/tla/ReplayWindow.tla:ReceivedIsRefused",
             "-", "-", "none"],
        ])
        with open(os.path.join(root, "proofs", "tla", "ReplayWindow.tla"), "w") as handle:
            handle.write("---- MODULE ReplayWindow ----\nReceivedIsRefused == TRUE\n====\n")
        code, report = check_witnesses.run(root)
        self.assertEqual(code, 0, report)

    def test_replay_row_property_is_defined_in_the_module_it_cites(self):
        root = os.path.abspath(os.path.join(HERE, "..", ".."))
        tsv = os.path.join(root, "docs", "WITNESSES.tsv")
        with open(tsv, encoding="utf-8") as handle:
            rows = [line.split("\t") for line in handle.read().splitlines() if line.strip()]
        # the replay row is found by its rule, never by its line: other
        # branches add witness rows above it
        matches = [cells for cells in rows
                   if len(cells) > 2 and cells[1].startswith("Refuse the packet without opening")]
        self.assertEqual(len(matches), 1, "WITNESSES.tsv must hold exactly one replay refusal row")
        cells = matches[0]
        cited = check_witnesses.split_module_citation(cells[2])
        if cited is None:
            module, name = "proofs/tla/ReplayWindow.tla", cells[2]
        else:
            module, name = cited
        self.assertTrue(
            check_witnesses.property_defined(root, module, name),
            "WITNESSES.tsv's replay refusal row names %s, which is not defined in %s" % (name, module),
        )

    def build_model(self, cfg_checks="INVARIANTS TypeOK FloorHolds", declared="invariant\tFloorHolds",
                    recorded="PASS\tinvariant\tFloorHolds", citation="FloorHolds", case_row=True):
        """A model row whose witness MCFloorBroken.cfg is a case of MCFloor.tla,
        which extends Floor.tla; Other.tla, which MCFloor does not extend,
        defines a property of the same name as Floor's StrayHolds."""
        root = self.build([
            ["t1", "the floor holds", citation, "-", "proofs/tla/MCFloorBroken.cfg", "model"],
        ])
        tla = os.path.join(root, "proofs", "tla")
        files = {
            "Floor.tla": "---- MODULE Floor ----\nTypeOK == TRUE\nFloorHolds == TRUE\n"
                         "Safety == TypeOK /\\ FloorHolds\n====\n",
            "MCFloor.tla": "---- MODULE MCFloor ----\nEXTENDS Floor, Naturals\n====\n",
            "Other.tla": "---- MODULE Other ----\nStrayHolds == TRUE\n====\n",
            "MCFloorBroken.cfg": "\\* reversed witness\nSPECIFICATION Spec\n%s\nCHECK_DEADLOCK FALSE\n" % cfg_checks,
            "CASES.tsv": "config\tmodule\texpected\tproperty\tdeadlock\tgroup\tgate\tdebt\n"
                         + ("MCFloorBroken.cfg\tMCFloor.tla\t%s\tcheck\tfloor\trequired\t-\n" % declared
                            if case_row else ""),
            "RUNS.tsv": "config\tmodule\tresult\texpected\tproperty\n"
                        "MCFloorBroken.cfg\tMCFloor.tla\t%s\n" % recorded,
        }
        for name, text in files.items():
            with open(os.path.join(tla, name), "w") as handle:
                handle.write(text)
        return root

    def test_model_witness_naming_and_violating_its_property_passes(self):
        code, report = check_witnesses.run(self.build_model())
        self.assertEqual(code, 0, report)
        code, report = check_witnesses.run(self.build_model(citation="Floor.tla:FloorHolds"))
        self.assertEqual(code, 0, report)
        # a listed definition that names the property counts as listing it
        code, report = check_witnesses.run(self.build_model(cfg_checks="INVARIANT Safety"))
        self.assertEqual(code, 0, report)

    def test_model_witness_whose_cfg_drops_the_property_fails(self):
        code, report = check_witnesses.run(self.build_model(cfg_checks="INVARIANTS TypeOK"))
        self.assertEqual(code, 1, report)
        self.assertIn("does not list FloorHolds", report)

    def test_model_property_defined_only_outside_the_case_module_fails(self):
        code, report = check_witnesses.run(self.build_model(
            cfg_checks="INVARIANTS StrayHolds", declared="invariant\tStrayHolds",
            recorded="PASS\tinvariant\tStrayHolds", citation="StrayHolds"))
        self.assertEqual(code, 1, report)
        self.assertIn("StrayHolds is not defined in MCFloor.tla", report)
        code, report = check_witnesses.run(self.build_model(citation="Other.tla:FloorHolds"))
        self.assertEqual(code, 1, report)
        self.assertIn("does not extend or instance", report)

    def test_model_witness_violating_another_property_fails(self):
        code, report = check_witnesses.run(self.build_model(
            declared="invariant\tTypeOK", recorded="PASS\tinvariant\tTypeOK"))
        self.assertEqual(code, 1, report)
        self.assertIn("declared to violate TypeOK, not FloorHolds", report)

    def test_model_witness_not_measured_as_declared_fails(self):
        code, report = check_witnesses.run(self.build_model(recorded="FAIL\tinvariant\tFloorHolds"))
        self.assertEqual(code, 1, report)
        self.assertIn("is recorded FAIL", report)

    def test_model_witness_with_no_case_row_fails(self):
        code, report = check_witnesses.run(self.build_model(case_row=False))
        self.assertEqual(code, 1, report)
        self.assertIn("has no proofs/tla/CASES.tsv row", report)

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
