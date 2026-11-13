#!/usr/bin/env python3
"""Tests for tools/docs/check_getting_started.py: a cited block that equals
its example range passes, one that differs is refused, and a prose-only
citation does not claim the block that follows the next citation in the same
section. Each case writes a guide and an example into a temporary directory
and runs check_guide on it."""

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_getting_started  # noqa: E402

EXAMPLE = "int one;\nint two;\nint three;\nint four;\n"


class CheckGettingStartedTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / "examples").mkdir()
        (self.root / "docs").mkdir()
        (self.root / "examples" / "hello.c").write_text(EXAMPLE)
        self.saved = check_getting_started.ROOT
        check_getting_started.ROOT = self.root

    def tearDown(self):
        check_getting_started.ROOT = self.saved
        self.tmp.cleanup()

    def check(self, guide_text):
        guide = self.root / "docs" / "GUIDE.md"
        guide.write_text(guide_text)
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = check_getting_started.check_guide(guide)
        return code, out.getvalue(), err.getvalue()

    def test_a_matching_block_passes(self):
        code, out, err = self.check(
            "## Start\n\n[examples/hello.c](../examples/hello.c), lines 3 to 4:\n\n"
            "```c\nint three;\nint four;\n```\n")
        self.assertEqual(code, 0, err)
        self.assertIn("OK: 1 cited block(s) match", out)

    def test_a_differing_block_is_refused(self):
        code, out, err = self.check(
            "## Start\n\n[examples/hello.c](../examples/hello.c), lines 3 to 4:\n\n"
            "```c\nint three;\nint five;\n```\n")
        self.assertEqual(code, 1, out)
        self.assertIn("block does not match examples/hello.c:3-4", err)

    def test_a_prose_citation_does_not_claim_the_next_citations_block(self):
        # the first citation is prose only; the block after the second one is
        # the second one's, though both sit in one section
        code, out, err = self.check(
            "## Start\n\nThe declarations are [examples/hello.c](../examples/hello.c), lines 1 to 2.\n\n"
            "[examples/hello.c](../examples/hello.c), lines 3 to 4:\n\n"
            "```c\nint three;\nint four;\n```\n")
        self.assertEqual(code, 0, err)
        self.assertIn("OK: 1 cited block(s) match", out)

    def test_a_prose_citation_outside_the_example_is_refused(self):
        code, out, err = self.check(
            "## Start\n\nSee [examples/hello.c](../examples/hello.c), lines 3 to 9.\n\n"
            "[examples/hello.c](../examples/hello.c), lines 1 to 2:\n\n"
            "```c\nint one;\nint two;\n```\n")
        self.assertEqual(code, 1, out)
        self.assertIn("cited range hello.c:3-9 lies outside the example (4 lines)", err)


if __name__ == "__main__":
    unittest.main()
