#!/usr/bin/env python3
"""tools/release.py and tools/release_gates.py against fixtures: every red refuses, by name.

Run: python3 tools/test_release.py -v
No network, no clock, no git: tools/release_testkit.py's FakeApi answers.
"""

import datetime
import io
import itertools
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import release  # noqa: E402
import release_gates as rg  # noqa: E402
from release_testkit import (ANCESTOR, NOW, OTHER, SHA, VERSION, FakeApi, green_release, header,  # noqa: E402
                             job, CI_JOBS, CI_SKIPPED)

REPO = "mas-bandwidth/rocketnet"


def run_release(api, version=VERSION, ref=SHA, dry_run=True, **kw):
    out = io.StringIO()
    ticks = itertools.count(0, 60)   # a minute per look: a wait that never ends hits the deadline
    code = release.release(api, REPO, version, ref, dry_run=dry_run, out=out, now=NOW,
                           sleep=lambda s: None, clock=kw.pop("clock", lambda: next(ticks)), **kw)
    return code, out.getvalue()


def ci_run(api):
    return next(r for r in api.runs if r["path"].endswith("/ci.yml"))


class Gates(unittest.TestCase):

    def test_the_repository_list_parses_and_is_not_empty(self):
        gates = rg.load_gates()
        self.assertGreater(len(gates), 10)
        kinds = {g.kind for g in gates}
        self.assertTrue({"run", "jobs", "job", "skip", "tlc-rerun", "tag"} <= kinds)

    def test_zero_gates_is_an_error(self):
        with self.assertRaises(rg.GatesError):
            rg.parse_gates("# only comments\n\n")

    def test_malformed_rows_are_refused(self):
        bad = [
            "ci.yml | - | sometimes | v1.1.8 | push | x",
            "ci.yml | - | run | 1.1.8 | push | x",
            "ci.yml | - | run | v1.1.8 | weekly | x",
            "ci.yml | a job | run | v1.1.8 | push | x",
            "ci.yml | - | job | v1.1.8 | push | x",
            "ci.yml | - | run | v1.1.8 | push",
            "x/ci.yml | - | run | v1.1.8 | push | x",
            "release-check.yml | a | tag | v1.1.8 | push | x",
        ]
        for row in bad:
            with self.subTest(row=row), self.assertRaises(rg.GatesError):
                rg.parse_gates(row)

    def test_version_order(self):
        order = ["v1.0.0-preview-5", "v1.0.0-preview-6", "v1.0.0-preview-10", "v1.0.0", "v1.1.7", "v1.1.8",
                 "v1.10.0"]
        keys = [rg.version_key(v) for v in order]
        self.assertEqual(keys, sorted(keys))


class Release(unittest.TestCase):

    def test_green_passes_and_dry_run_touches_nothing(self):
        api = green_release(FakeApi())
        code, text = run_release(api)
        self.assertEqual(code, 0, text)
        self.assertIn("VERDICT: PASS v1.1.8", text)
        self.assertIn("PASS  ci.yml [jobs, push,pull_request, from v1.1.8]", text)
        self.assertIn("'MemorySanitizer': success", text)
        self.assertIn("dry run: nothing tagged", text)
        self.assertEqual(api.writes, [])

    def test_cancelled_job_refuses(self):
        api = green_release(FakeApi())
        api.jobs[ci_run(api)["id"]][0] = job("linux / Debug / unit", "cancelled", secs=124)
        code, text = run_release(api)
        self.assertEqual(code, 1)
        self.assertIn("RED: linux / Debug / unit: cancelled (124 s)", text)
        self.assertIn("VERDICT: REFUSE", text)

    def test_cancelled_job_refuses_even_when_the_run_reads_success(self):
        api = green_release(FakeApi())
        api.jobs[ci_run(api)["id"]][0] = job("linux / Debug / unit", "cancelled", secs=124)
        ci_run(api)["conclusion"] = "success"
        code, text = run_release(api)
        self.assertEqual(code, 1)
        self.assertIn("linux / Debug / unit: cancelled", text)

    def test_timed_out_refuses(self):
        api = green_release(FakeApi())
        api.jobs[ci_run(api)["id"]][1] = job("lint (workflows, format, standard, tool selftests)", "timed_out")
        code, text = run_release(api)
        self.assertEqual(code, 1)
        self.assertIn("lint (workflows, format, standard, tool selftests): timed_out", text)

    def test_failure_refuses(self):
        api = green_release(FakeApi())
        api.jobs[ci_run(api)["id"]][0] = job("linux / Debug / unit", "failure")
        code, text = run_release(api)
        self.assertEqual(code, 1)
        self.assertIn("linux / Debug / unit: failure", text)

    def test_missing_required_job_refuses(self):
        api = green_release(FakeApi())
        del api.jobs[ci_run(api)["id"]][1]
        code, text = run_release(api)
        self.assertEqual(code, 1)
        self.assertIn("MISSING: lint (workflows, format, standard, tool selftests)", text)

    def test_skipped_without_a_skip_row_refuses(self):
        api = green_release(FakeApi())
        api.jobs[ci_run(api)["id"]][0] = job("linux / Debug / unit", "skipped", secs=0)
        code, text = run_release(api)
        self.assertEqual(code, 1)
        self.assertIn("linux / Debug / unit: skipped (no skip row in tools/release_gates.txt)", text)

    def test_listed_skip_passes(self):
        api = green_release(FakeApi())
        code, text = run_release(api)
        self.assertEqual(code, 0, text)
        self.assertIn("2 skipped as listed", text)

    def test_in_progress_refuses(self):
        api = green_release(FakeApi())
        api.add_run("ci.yml", [job(n, status="in_progress") for n in CI_JOBS], SHA, status="in_progress",
                    started=NOW - datetime.timedelta(minutes=5))
        code, text = run_release(api)
        self.assertEqual(code, 1)
        self.assertIn("IN PROGRESS", text)

    def test_in_progress_job_refuses(self):
        api = green_release(FakeApi())
        api.jobs[ci_run(api)["id"]][0] = job("linux / Debug / unit", status="in_progress")
        code, text = run_release(api)
        self.assertEqual(code, 1)
        self.assertIn("linux / Debug / unit: in_progress", text)

    def test_runs_for_another_sha_do_not_count(self):
        api = green_release(FakeApi(), sha=OTHER)
        api.commits[OTHER] = OTHER
        api.files[("sdk/rocketnet.h", SHA)] = header(VERSION)
        api.files[(f"docs/RELEASE-NOTES-{VERSION}.md", SHA)] = "notes"
        code, text = run_release(api)
        self.assertEqual(code, 1)
        self.assertIn("MISSING: no push run of ci.yml on aaaaaaaaaa", text)

    def test_zero_runs_refuses(self):
        api = FakeApi()
        api.files[("sdk/rocketnet.h", SHA)] = header(VERSION)
        api.files[(f"docs/RELEASE-NOTES-{VERSION}.md", SHA)] = "notes"
        code, text = run_release(api)
        self.assertEqual(code, 1)
        self.assertGreaterEqual(text.count("MISSING"), 8)

    def test_commit_not_on_main_refuses(self):
        api = green_release(FakeApi())
        api.ancestors.discard((SHA, api.commits["main"]))
        code, text = run_release(api)
        self.assertEqual(code, 1)
        self.assertIn("FAIL  commit is on main", text)

    def test_version_mismatch_refuses(self):
        api = green_release(FakeApi())
        api.files[("sdk/rocketnet.h", SHA)] = header("v1.1.7")
        code, text = run_release(api)
        self.assertEqual(code, 1)
        self.assertIn("ROCKETNET_VERSION_FULL is 'v1.1.7', want 'v1.1.8'", text)

    def test_bad_version_refuses(self):
        code, text = run_release(green_release(FakeApi()), version="1.1.8")
        self.assertEqual(code, 1)
        self.assertIn("is not vMAJOR.MINOR.PATCH", text)

    def test_missing_notes_refuse(self):
        api = green_release(FakeApi())
        del api.files[(f"docs/RELEASE-NOTES-{VERSION}.md", SHA)]
        code, text = run_release(api)
        self.assertEqual(code, 1)
        self.assertIn("FAIL  release notes docs/RELEASE-NOTES-v1.1.8.md", text)

    def test_tag_elsewhere_refuses(self):
        api = green_release(FakeApi())
        api.tags[VERSION] = OTHER
        code, text = run_release(api)
        self.assertEqual(code, 1)
        self.assertIn("a tag is never moved", text)

    def test_newest_completed_run_decides(self):
        api = green_release(FakeApi())
        api.add_run("cbmc.yml", [job("cbmc (arithmetic leaf proofs)", "failure")], SHA, "schedule",
                    started=NOW - datetime.timedelta(minutes=10))
        code, text = run_release(api)
        self.assertEqual(code, 1)
        self.assertIn("(newest completed of 2 runs decides): failure", text)

    def test_newer_success_after_failure_passes(self):
        api = green_release(FakeApi())
        api.add_run("cbmc.yml", [job("cbmc (arithmetic leaf proofs)", "failure")], SHA, "schedule",
                    started=NOW - datetime.timedelta(days=1))
        code, text = run_release(api)
        self.assertEqual(code, 0, text)
        self.assertIn("(newest completed of 2 runs decides): success", text)

    def test_nightly_ci_every_job(self):
        api = green_release(FakeApi())
        run = next(r for r in api.runs if r["path"].endswith("/nightly-ci.yml"))
        api.jobs[run["id"]][2] = job("full / unit-case coverage", "skipped", secs=0)
        code, text = run_release(api)
        self.assertEqual(code, 1)
        self.assertIn("full / unit-case coverage: skipped (no skip row", text)

    def test_msan_red_refuses_from_v1_1_8(self):
        api = green_release(FakeApi())
        run = next(r for r in api.runs if r["path"].endswith("/msan.yml"))
        api.jobs[run["id"]][0] = job("MemorySanitizer", "failure")
        code, text = run_release(api)
        self.assertEqual(code, 1)
        self.assertIn("'MemorySanitizer': failure", text)

    def test_soak_red_refuses_from_v1_1_8(self):
        # v1.1.7 was tagged 2.5 h after soak per transport went red: no gate read nightly-soak.yml
        api = green_release(FakeApi())
        run = next(r for r in api.runs if r["path"].endswith("/nightly-soak.yml"))
        api.jobs[run["id"]][1] = job("soak per transport (main / 1800 s per cell)", "failure")
        code, text = run_release(api)
        self.assertEqual(code, 1)
        self.assertIn("FAIL  nightly-soak.yml [jobs, nightly, from v1.1.8]", text)

    def test_nightly_functional_tier_missing_refuses(self):
        api = green_release(FakeApi())
        run = next(r for r in api.runs if r["path"].endswith("/nightly.yml"))
        api.jobs[run["id"]] = [j for j in api.jobs[run["id"]] if not j["name"].startswith("windows / Release")]
        code, text = run_release(api)
        self.assertEqual(code, 1)
        self.assertIn("no nightly run of nightly.yml on", text)
        self.assertIn("windows / Release functional nightly tier", text)

    def test_gates_as_of_judges_an_earlier_release_by_later_gates(self):
        api = green_release(FakeApi(), version="v1.1.7")
        api.runs = [r for r in api.runs if not r["path"].endswith("/nightly-soak.yml")]
        out = io.StringIO()
        gates = release.gates_as_of(rg.load_gates(), "v1.1.8", out)
        code, text = run_release(api, version="v1.1.7", gates=gates)
        self.assertEqual(code, 1, text)
        self.assertIn("GATES AS OF v1.1.8", out.getvalue())
        self.assertIn("MISSING: no nightly run of nightly-soak.yml", text)
        self.assertNotIn("NOT IN FORCE", text)

    def test_gates_as_of_is_dry_run_only(self):
        self.assertEqual(release.main(["release.py", "v1.1.7", "76edea48e", "--gates-as-of", "v1.1.8"]), 2)

    def test_earlier_version_is_judged_by_the_gates_in_force_then(self):
        api = green_release(FakeApi(), version="v1.1.7")
        api.runs = [r for r in api.runs if not r["path"].endswith("/msan.yml")]
        api.jobs[ci_run(api)["id"]][0] = job("linux / Debug / unit", "cancelled", secs=125)
        ci_run(api)["conclusion"] = "cancelled"
        code, text = run_release(api, version="v1.1.7")
        # v1.1.7: the run-level rule; the cancelled job made the run cancelled, so it is still red
        self.assertEqual(code, 1)
        self.assertIn("NOT IN FORCE for v1.1.7: msan.yml 'MemorySanitizer'", text)
        self.assertNotIn("MemorySanitizer': ", text)

    def test_earlier_version_with_green_runs_passes_without_the_new_gates(self):
        api = green_release(FakeApi(), version="v1.1.7")
        api.runs = [r for r in api.runs if not r["path"].endswith(("/msan.yml", "/tlc.yml"))]
        api.add_run("tlc.yml", [job("Model records are current")], SHA, "workflow_dispatch")
        code, text = run_release(api, version="v1.1.7")
        self.assertEqual(code, 0, text)
        self.assertIn("NOT IN FORCE for v1.1.7: tlc.yml 'TLC re-run'", text)

    def test_v1_0_0_shape_is_refused(self):
        # v1.0.0 (9904027045): CI push run failure; Nightly CI and TLC records never green
        api = green_release(FakeApi(), version="v1.0.0")
        for r in api.runs:
            if r["path"].endswith(("/ci.yml", "/nightly-ci.yml", "/tlc.yml")):
                r["conclusion"] = "failure"
        code, text = run_release(api, version="v1.0.0")
        self.assertEqual(code, 1)
        self.assertIn("FAIL  ci.yml [run, push", text)
        self.assertIn("FAIL  nightly-ci.yml [run, nightly", text)
        self.assertIn("FAIL  tlc.yml [run, nightly", text)


class TlcRerun(unittest.TestCase):

    def slices(self, api, n, ks, sha=ANCESTOR, age_days=1, conclusion="success"):
        created = NOW - datetime.timedelta(days=age_days)
        for k in ks:
            api.add_run("tlc.yml", [job("Model records are current"), job(f"TLC re-run slice {k}/{n}", conclusion)],
                        sha, "schedule", created=created, started=created)

    def without_full(self, api):
        run = next(r for r in api.runs if r["path"].endswith("/tlc.yml") and r["head_sha"] == SHA)
        api.jobs[run["id"]] = [job("Model records are current")]

    def test_full_rerun_on_the_exact_commit_passes(self):
        code, text = run_release(green_release(FakeApi()))
        self.assertEqual(code, 0, text)
        self.assertIn("'TLC re-run full': success on the exact commit", text)

    def test_every_slice_on_ancestors_passes(self):
        api = green_release(FakeApi())
        self.without_full(api)
        self.slices(api, 3, [0, 1, 2])
        code, text = run_release(api)
        self.assertEqual(code, 0, text)
        self.assertIn("every slice of the last 3 days succeeded on an ancestor", text)

    def test_a_missing_slice_refuses(self):
        api = green_release(FakeApi())
        self.without_full(api)
        self.slices(api, 3, [0, 2])
        code, text = run_release(api)
        self.assertEqual(code, 1)
        self.assertIn("slices of N=3 missing in the last 3 days on ancestors: 1/3", text)

    def test_a_slice_on_a_commit_that_is_not_an_ancestor_refuses(self):
        api = green_release(FakeApi())
        self.without_full(api)
        self.slices(api, 2, [0])
        self.slices(api, 2, [1], sha=OTHER)
        code, text = run_release(api)
        self.assertEqual(code, 1)
        self.assertIn("1/2", text)

    def test_a_slice_older_than_n_days_refuses(self):
        api = green_release(FakeApi())
        self.without_full(api)
        self.slices(api, 2, [0], age_days=1)
        self.slices(api, 2, [1], age_days=5)
        code, text = run_release(api)
        self.assertEqual(code, 1)

    def test_no_rerun_evidence_refuses(self):
        api = green_release(FakeApi())
        self.without_full(api)
        code, text = run_release(api)
        self.assertEqual(code, 1)
        self.assertIn("no 'TLC re-run full' job", text)

    def test_failed_full_rerun_refuses(self):
        api = green_release(FakeApi())
        run = next(r for r in api.runs if r["path"].endswith("/tlc.yml"))
        api.jobs[run["id"]][1] = job("TLC re-run full", "failure")
        code, text = run_release(api)
        self.assertEqual(code, 1)
        self.assertIn("'TLC re-run full' in run", text)


class Publish(unittest.TestCase):

    def tag_run(self, conclusion):
        def on_tag(api, version):
            api.add_run("release-check.yml", [job("version consistency"),
                                              job("threaded tsan run on the release commit", conclusion),
                                              job("release gates passed on the tag", conclusion)],
                        SHA, "push", version, started=NOW)
        return on_tag

    def test_green_tags_waits_and_publishes(self):
        api = green_release(FakeApi())
        api.on_tag = self.tag_run("success")
        code, text = run_release(api, dry_run=False)
        self.assertEqual(code, 0, text)
        paths = [w[1] for w in api.writes]
        self.assertEqual(paths, [f"/repos/{REPO}/git/tags", f"/repos/{REPO}/git/refs", f"/repos/{REPO}/releases"])
        body = api.writes[2][2]
        self.assertEqual((body["tag_name"], body["make_latest"], body["prerelease"]), (VERSION, "true", False))
        self.assertEqual(body["body"], f"# rocketnet {VERSION}\n")
        self.assertIn("'threaded tsan run on the release commit': success", text)

    def test_red_tag_run_is_not_published(self):
        api = green_release(FakeApi())
        api.on_tag = self.tag_run("failure")
        code, text = run_release(api, dry_run=False)
        self.assertEqual(code, 1)
        self.assertNotIn(f"/repos/{REPO}/releases", [w[1] for w in api.writes])
        self.assertIn("not publishing v1.1.8", text)

    def test_tag_run_that_never_comes_is_not_published(self):
        api = green_release(FakeApi())
        ticks = iter(range(0, 100000, 600))
        code, text = run_release(api, dry_run=False, clock=lambda: next(ticks), wait_minutes=5)
        self.assertEqual(code, 1)
        self.assertNotIn(f"/repos/{REPO}/releases", [w[1] for w in api.writes])

    def test_red_gate_tags_nothing(self):
        api = green_release(FakeApi())
        api.jobs[ci_run(api)["id"]][0] = job("linux / Debug / unit", "cancelled", secs=124)
        code, _ = run_release(api, dry_run=False)
        self.assertEqual(code, 1)
        self.assertEqual(api.writes, [])

    def test_existing_tag_needs_resume(self):
        api = green_release(FakeApi())
        api.tags[VERSION] = SHA
        code, text = run_release(api, dry_run=False)
        self.assertEqual(code, 1)
        self.assertIn("use --resume", text)
        api.add_run("release-check.yml", [job("threaded tsan run on the release commit"),
                                          job("release gates passed on the tag")], SHA, "push", VERSION,
                    started=NOW)
        code, text = run_release(api, dry_run=False, resume=True)
        self.assertEqual(code, 0, text)
        self.assertEqual([w[1] for w in api.writes], [f"/repos/{REPO}/releases"])


if __name__ == "__main__":
    unittest.main()
