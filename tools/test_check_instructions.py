#!/usr/bin/env python3
"""
Unit tests for tools/check_instructions.py covering DONE-WHEN (5):
1. Parse valid and malformed cachegrind output and scenario stdout.
2. PASS when every region is at or within +2.0% of baseline.
3. FAIL (exit 1) when any region is more than +2.0% over baseline.
4. FAIL (exit 1, STALE BASELINE) when a region is more than 2.0% under baseline; PASS at 2.0% under.
5. Toolchain mismatch (toolchain_id, compiler_id, valgrind_version, or build_type) fails naming the mismatch;
   a distro or host build reporting the same compiler_id is not comparable to a pinned baseline.
6. Repin (--repin --reason <text>) rewrites baseline JSON with counts, toolchain metadata, commit, and reason, and refuses missing --reason or an unpinned toolchain.
"""

import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_instructions as ci

PINNED = "pinned-gcc-13.3.0-valgrind-3.26.0-script-0123456789ab"


def make_cachegrind_out(ir: int, events: str = "Ir") -> str:
    return (
        "desc: I1 cache: 32768 B, 64 B, 8-way associative\n"
        f"cmd: ./bin/instruction_count\n"
        f"events: {events}\n"
        "fl=benchmarks/instruction_count.cpp\n"
        "fn=main\n"
        f"1 {ir}\n"
        f"summary: {ir}\n"
    )


def make_baseline(
    client_ir: int = 1000000,
    client_units: int = 100,
    server_ir: int = 2000000,
    server_units: int = 100,
    compiler_id: str = "GNU-13.3.0",
    valgrind_version: str = "valgrind-3.26.0",
    build_type: str = "Release",
    toolchain_id: str = PINNED,
) -> dict:
    return {
        "toolchain_id": toolchain_id,
        "compiler_id": compiler_id,
        "valgrind_version": valgrind_version,
        "build_type": build_type,
        "commit": "0123456789abcdef0123456789abcdef01234567",
        "reason": "initial baseline pin",
        "threshold_pct": 2.0,
        "regions": {
            "client_frame": {
                "ir": client_ir,
                "units": client_units,
                "per_unit": round(client_ir / client_units, 2),
            },
            "server_tick": {
                "ir": server_ir,
                "units": server_units,
                "per_unit": round(server_ir / server_units, 2),
            },
        },
    }


class TestCheckInstructions(unittest.TestCase):
    def test_parse_cachegrind_and_stdout(self) -> None:
        self.assertEqual(ci.parse_cachegrind_ir(make_cachegrind_out(123456)), 123456)
        self.assertEqual(
            ci.parse_cachegrind_ir(
                "events: Dr Ir Dw\nsummary: 10 987654 20\n"
            ),
            987654,
        )
        with self.assertRaises(ValueError):
            ci.parse_cachegrind_ir("events: Ir\nno summary line here\n")
        with self.assertRaises(ValueError):
            ci.parse_cachegrind_ir("events: Ir\nsummary: not_an_int\n")

        meta = ci.parse_scenario_stdout(
            "instruction_count region=client_frame units=100 compiler_id=GNU-13.3.0 build_type=Release\n"
        )
        self.assertEqual(meta["region"], "client_frame")
        self.assertEqual(meta["units"], 100)
        self.assertEqual(meta["compiler_id"], "GNU-13.3.0")
        self.assertEqual(meta["build_type"], "Release")

    def test_pass_within_two_percent(self) -> None:
        baseline = make_baseline(client_ir=1000000, server_ir=2000000)
        measured = {
            "client_frame": {"ir": 1015000, "units": 100},
            "server_tick": {"ir": 2040000, "units": 100},
        }
        code, report = ci.evaluate_instructions(
            baseline=baseline,
            measured=measured,
            compiler_id="GNU-13.3.0",
            valgrind_version="valgrind-3.26.0",
            build_type="Release",
            toolchain_id=PINNED,
        )
        self.assertEqual(code, 0, msg=report)
        self.assertIn(
            "instructions region=client_frame ir=1015000 per_unit=10150.00 baseline=1000000 delta_pct=+1.50% verdict=PASS",
            report,
        )
        self.assertIn(
            "instructions region=server_tick ir=2040000 per_unit=20400.00 baseline=2000000 delta_pct=+2.00% verdict=PASS",
            report,
        )
        self.assertNotIn("verdict=FAIL", report)

    def test_zero_regions_fails(self) -> None:
        # zero inputs is an error: nothing measured, or a baseline with no regions, never passes
        with self.assertRaises(ValueError):
            ci.parse_scenario_stdout("")
        with self.assertRaises(ValueError):
            ci.parse_cachegrind_ir("")
        for baseline, measured in (
            (make_baseline(), {}),
            (dict(make_baseline(), regions={}),
             {"client_frame": {"ir": 1000000, "units": 100}, "server_tick": {"ir": 2000000, "units": 100}}),
        ):
            code, report = ci.evaluate_instructions(
                baseline=baseline,
                measured=measured,
                compiler_id="GNU-13.3.0",
                valgrind_version="valgrind-3.26.0",
                build_type="Release",
                toolchain_id=PINNED,
            )
            self.assertEqual(code, 1, msg=report)
            self.assertIn("verdict=FAIL (missing", report)
            self.assertNotIn("verdict=PASS", report)

    def test_fail_over_two_percent_threshold(self) -> None:
        baseline = make_baseline(client_ir=1000000, server_ir=2000000)
        measured = {
            "client_frame": {"ir": 1020001, "units": 100},
            "server_tick": {"ir": 2000000, "units": 100},
        }
        code, report = ci.evaluate_instructions(
            baseline=baseline,
            measured=measured,
            compiler_id="GNU-13.3.0",
            valgrind_version="valgrind-3.26.0",
            build_type="Release",
            toolchain_id=PINNED,
        )
        self.assertEqual(code, 1, msg=report)
        self.assertIn("region=client_frame", report)
        self.assertIn("verdict=FAIL", report)

    def test_under_baseline_fails_as_stale(self) -> None:
        # more than 2.0% under: the baseline is stale-high (growth back up to
        # it would pass unseen), so the gate fails until it is re-pinned
        baseline = make_baseline(client_ir=1000000, server_ir=2000000)
        measured = {
            "client_frame": {"ir": 950000, "units": 100},
            "server_tick": {"ir": 2000000, "units": 100},
        }
        code, report = ci.evaluate_instructions(
            baseline=baseline,
            measured=measured,
            compiler_id="GNU-13.3.0",
            valgrind_version="valgrind-3.26.0",
            build_type="Release",
            toolchain_id=PINNED,
        )
        self.assertEqual(code, 1, msg=report)
        self.assertIn("STALE BASELINE: region=client_frame", report)
        self.assertIn("region=client_frame ir=950000", report)
        self.assertIn("verdict=FAIL", report)

    def test_at_two_percent_under_passes(self) -> None:
        baseline = make_baseline(client_ir=1000000, server_ir=2000000)
        measured = {
            "client_frame": {"ir": 980000, "units": 100},
            "server_tick": {"ir": 1960001, "units": 100},
        }
        code, report = ci.evaluate_instructions(
            baseline=baseline,
            measured=measured,
            compiler_id="GNU-13.3.0",
            valgrind_version="valgrind-3.26.0",
            build_type="Release",
            toolchain_id=PINNED,
        )
        self.assertEqual(code, 0, msg=report)
        self.assertNotIn("STALE", report)
        self.assertNotIn("verdict=FAIL", report)

    def test_toolchain_mismatch_fails_naming_mismatch(self) -> None:
        baseline = make_baseline()
        measured = {
            "client_frame": {"ir": 1000000, "units": 100},
            "server_tick": {"ir": 2000000, "units": 100},
        }
        for field, kwargs in [
            ("toolchain_id", {"compiler_id": "GNU-13.3.0", "valgrind_version": "valgrind-3.26.0", "build_type": "Release", "toolchain_id": ci.UNPINNED_TOOLCHAIN_ID}),
            ("compiler_id", {"compiler_id": "GNU-14.1.0", "valgrind_version": "valgrind-3.26.0", "build_type": "Release", "toolchain_id": PINNED}),
            ("valgrind_version", {"compiler_id": "GNU-13.3.0", "valgrind_version": "valgrind-3.22.0", "build_type": "Release", "toolchain_id": PINNED}),
            ("build_type", {"compiler_id": "GNU-13.3.0", "valgrind_version": "valgrind-3.26.0", "build_type": "Debug", "toolchain_id": PINNED}),
        ]:
            code, report = ci.evaluate_instructions(
                baseline=baseline,
                measured=measured,
                **kwargs,
            )
            self.assertEqual(code, 1, msg=report)
            self.assertIn(field, report)
            self.assertIn("verdict=FAIL", report)

    def test_same_compiler_id_without_the_pinned_toolchain_is_not_comparable(self) -> None:
        baseline = make_baseline()
        self.assertIsNone(ci.check_toolchain_comparable(baseline, "GNU-13.3.0", "valgrind-3.26.0", PINNED))
        msg = ci.check_toolchain_comparable(baseline, "GNU-13.3.0", "valgrind-3.26.0", ci.UNPINNED_TOOLCHAIN_ID)
        self.assertIsNotNone(msg)
        self.assertIn(PINNED, msg or "")
        other = PINNED[:-12] + "ba9876543210"
        self.assertIsNotNone(ci.check_toolchain_comparable(baseline, "GNU-13.3.0", "valgrind-3.26.0", other))
        legacy = make_baseline()
        del legacy["toolchain_id"]
        self.assertIsNotNone(ci.check_toolchain_comparable(legacy, "GNU-13.3.0", "valgrind-3.26.0", PINNED))
        self.assertIsNotNone(ci.check_toolchain_comparable(legacy, "GNU-13.3.0", "valgrind-3.26.0", ""))

    def test_repin_writes_baseline_and_requires_reason(self) -> None:
        measured = {
            "client_frame": {"ir": 1234500, "units": 100},
            "server_tick": {"ir": 2345600, "units": 100},
        }
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "instruction_baseline.json")
            with self.assertRaises(ValueError):
                ci.write_repin_baseline(
                    baseline_path=path,
                    measured=measured,
                    compiler_id="GNU-13.3.0",
                    valgrind_version="valgrind-3.26.0",
                    build_type="Release",
                    toolchain_id=PINNED,
                    commit="abcdef0123456789abcdef0123456789abcdef01",
                    reason="   ",
                )
            with self.assertRaises(ValueError):
                ci.write_repin_baseline(
                    baseline_path=path,
                    measured=measured,
                    compiler_id="GNU-13.3.0",
                    valgrind_version="valgrind-3.26.0",
                    build_type="Release",
                    toolchain_id=ci.UNPINNED_TOOLCHAIN_ID,
                    commit="abcdef0123456789abcdef0123456789abcdef01",
                    reason="pin after loopback batch change",
                )
            ci.write_repin_baseline(
                baseline_path=path,
                measured=measured,
                compiler_id="GNU-13.3.0",
                valgrind_version="valgrind-3.26.0",
                build_type="Release",
                toolchain_id=PINNED,
                commit="abcdef0123456789abcdef0123456789abcdef01",
                reason="pin after loopback batch change",
            )
            with open(path, "r", encoding="utf-8") as f:
                saved = json.load(f)
            self.assertEqual(saved["toolchain_id"], PINNED)
            self.assertEqual(saved["compiler_id"], "GNU-13.3.0")
            self.assertEqual(saved["valgrind_version"], "valgrind-3.26.0")
            self.assertEqual(saved["build_type"], "Release")
            self.assertEqual(saved["commit"], "abcdef0123456789abcdef0123456789abcdef01")
            self.assertEqual(saved["reason"], "pin after loopback batch change")
            self.assertEqual(saved["regions"]["client_frame"]["ir"], 1234500)
            self.assertEqual(saved["regions"]["server_tick"]["ir"], 2345600)
            with self.assertRaises(ValueError):
                ci.write_repin_baseline(
                    baseline_path=path,
                    measured=measured,
                    compiler_id="GNU-13.3.0",
                    valgrind_version="valgrind-3.26.0",
                    build_type="Release",
                    toolchain_id=PINNED,
                    commit="unknown",
                    reason="a re-pin outside any checkout",
                )

    def test_baseline_commit_must_be_an_ancestor_of_head(self) -> None:
        # a git history in a temp dir: main has A <- B; side branches off A
        # with S, then main is checked out again (S is the rebased-away
        # measurement the 2026-10-31 baseline recorded)
        import subprocess

        with tempfile.TemporaryDirectory() as repo:
            env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.invalid",
                       GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.invalid",
                       GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")

            def git(*args: str) -> str:
                return subprocess.run(["git", *args], cwd=repo, env=env, check=True,
                                      capture_output=True, text=True).stdout.strip()

            git("init", "-q", "-b", "main")
            with open(os.path.join(repo, "f"), "w", encoding="utf-8") as f:
                f.write("a\n")
            git("add", "f")
            git("commit", "-q", "-m", "A")
            a = git("rev-parse", "HEAD")
            git("checkout", "-q", "-b", "side")
            git("commit", "-q", "--allow-empty", "-m", "S")
            s = git("rev-parse", "HEAD")
            git("checkout", "-q", "main")
            git("commit", "-q", "--allow-empty", "-m", "B")
            b = git("rev-parse", "HEAD")

            self.assertIsNone(ci.baseline_commit_problem(a, repo))
            self.assertIsNone(ci.baseline_commit_problem(b, repo))
            self.assertIn("not an ancestor", ci.baseline_commit_problem(s, repo) or "")
            self.assertIn("not in this history", ci.baseline_commit_problem("1" * 40, repo) or "")
            self.assertIn("no full commit hash", ci.baseline_commit_problem("unknown", repo) or "")
            self.assertIn("no full commit hash", ci.baseline_commit_problem(a[:9], repo) or "")

            # a re-pin records HEAD, so the tree must be what HEAD holds; the
            # baseline file itself may differ
            baseline = os.path.join(repo, "instruction_baseline.json")
            self.assertIsNone(ci.repin_refusal(baseline, {"GITHUB_REF": "refs/heads/main"}, repo))
            self.assertIn("merge ref", ci.repin_refusal(baseline, {"GITHUB_REF": "refs/pull/798/merge"}, repo) or "")
            with open(os.path.join(repo, "f"), "w", encoding="utf-8") as f:
                f.write("changed\n")
            self.assertIn("tree has changes", ci.repin_refusal(baseline, {}, repo) or "")


if __name__ == "__main__":
    unittest.main()
