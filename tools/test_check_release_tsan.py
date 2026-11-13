#!/usr/bin/env python3
"""Drive tools/check_release_tsan.py (Release Check's tag-push second check) with fixtures.

It judges the tag through tools/release_gates.py and tools/release_gates.txt,
the list tools/release.py reads before tagging; tools/test_release.py holds
the per-gate cases. These cases prove the second check reads that list, says
red on the tag, and needs a complete environment.

Run: python3 tools/test_check_release_tsan.py -v
No network: every API answer is a fixture.
"""

import io
import os
import sys
import unittest
import unittest.mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_release_tsan  # noqa: E402
from release_testkit import NOW, REPO, SHA, VERSION, FakeApi, green_release, job  # noqa: E402


def verdict(api, version=VERSION):
    out = io.StringIO()
    code = check_release_tsan.check(api, REPO, SHA, version, out, now=NOW)
    return code, out.getvalue()


def run_of(api, workflow):
    return next(r for r in api.runs if r["path"].endswith("/" + workflow))


class CheckReleaseTsan(unittest.TestCase):

    def test_green_passes(self):
        api = green_release(FakeApi())
        api.tags[VERSION] = SHA
        code, text = verdict(api)
        self.assertEqual(code, 0, text)
        self.assertIn("'tsan (threaded transport) (main)': success", text)
        self.assertIn("VERDICT: PASS", text)

    def test_point_release_tag_is_judged_by_the_per_commit_tier_only(self):
        api = green_release(FakeApi(), version="v1.1.8")
        api.tags["v1.1.8"] = SHA
        api.jobs[run_of(api, "nightly.yml")["id"]][1] = job("tsan (threaded transport) (main)", "failure")
        code, text = verdict(api, "v1.1.8")
        self.assertEqual(code, 0, text)
        self.assertIn("MINOR ONLY nightly.yml 'tsan (threaded transport) (<ref>)'", text)
        self.assertIn("VERDICT: PASS", text)

    def test_minor_release_tag_is_judged_by_the_nightly_tier(self):
        api = green_release(FakeApi(), version="v1.2.0")
        api.tags["v1.2.0"] = SHA
        api.jobs[run_of(api, "nightly.yml")["id"]][1] = job("tsan (threaded transport) (main)", "failure")
        code, text = verdict(api, "v1.2.0")
        self.assertEqual(code, 1)
        self.assertIn("'tsan (threaded transport) (main)': failure", text)

    def test_tag_stage_does_not_ask_for_the_tag_or_notes(self):
        api = green_release(FakeApi())
        del api.files[(f"docs/RELEASE-NOTES-{VERSION}.md", SHA)]
        code, text = verdict(api)
        self.assertEqual(code, 0, text)
        self.assertNotIn("RELEASE-NOTES", text)

    def test_failed_tsan_leg_fails(self):
        api = green_release(FakeApi())
        api.jobs[run_of(api, "nightly.yml")["id"]][1] = job("tsan (threaded transport) (main)", "failure")
        code, text = verdict(api)
        self.assertEqual(code, 1)
        self.assertIn("'tsan (threaded transport) (main)': failure", text)
        self.assertIn("::error::Release gates are not green", text)

    def test_leg_for_another_ref_does_not_count(self):
        api = green_release(FakeApi())
        api.jobs[run_of(api, "nightly.yml")["id"]][1] = job("tsan (threaded transport) (rowan/feature)")
        code, text = verdict(api)
        self.assertEqual(code, 1)
        self.assertIn("MISSING", text)

    def test_missing_nightly_ci_fails(self):
        api = green_release(FakeApi())
        api.runs.remove(run_of(api, "nightly-ci.yml"))
        code, text = verdict(api)
        self.assertEqual(code, 1)
        self.assertIn("MISSING: no nightly run of nightly-ci.yml", text)

    def test_cancelled_per_commit_job_fails_on_the_tag_too(self):
        api = green_release(FakeApi())
        api.jobs[run_of(api, "ci.yml")["id"]][0] = job("linux / Debug / unit", "cancelled", secs=125)
        code, text = verdict(api)
        self.assertEqual(code, 1)
        self.assertIn("linux / Debug / unit: cancelled (125 s)", text)

    def test_no_ruleset_at_the_commit_fails_by_name(self):
        # the required job names are judged or the release refuses: never skipped in silence
        api = green_release(FakeApi(), branch_ruleset=False)
        api.tags[VERSION] = SHA
        code, text = verdict(api)
        self.assertEqual(code, 1, text)
        self.assertIn(".github/rulesets/main.json is not at the commit", text)

    def test_tag_that_is_not_the_header_version_fails(self):
        code, text = verdict(green_release(FakeApi()), version="v1.1.9")
        self.assertEqual(code, 1)
        self.assertIn("want 'v1.1.9'", text)

    def test_zero_gates_in_force_fails(self):
        # zero inputs is an error: a list with no gate in force for the tag judges nothing
        for gates in ([], [g for g in check_release_tsan.rg.load_gates() if g.kind == "tag"]):
            out = io.StringIO()
            code = check_release_tsan.check(green_release(FakeApi()), REPO, SHA, VERSION, out, gates=gates, now=NOW)
            self.assertEqual(code, 1, out.getvalue())
            self.assertIn("0 release gates in force", out.getvalue())

    def test_main_refuses_an_incomplete_environment(self):
        saved = dict(os.environ)
        try:
            for name in ("GITHUB_REPOSITORY", "GITHUB_SHA", "GITHUB_REF_NAME", "GITHUB_TOKEN"):
                os.environ.pop(name, None)
            with unittest.mock.patch("sys.stdout", new_callable=io.StringIO):
                self.assertEqual(check_release_tsan.main(), 2)
        finally:
            os.environ.clear()
            os.environ.update(saved)


if __name__ == "__main__":
    unittest.main()
