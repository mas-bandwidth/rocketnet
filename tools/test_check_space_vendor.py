#!/usr/bin/env python3
"""Tests for the space vendor gate in tools/check_space_vendor.py.

Every fixture is a temporary git repository built by the test, or a canned
answer for the API mode. No test opens a socket, reads a clock, starts a
thread or reaches the network: the API mode is driven through a stand-in for
urllib.request.urlopen that answers from data the test wrote.

The cases are:

  test_identical_vendor_passes    the application vendors the release commit
                                  byte for byte: the gate passes, in the clone
                                  mode and in the API mode
  test_changed_byte_fails         one vendored file has different bytes
  test_missing_file_fails         one vendored file is absent
  test_extra_file_fails           the application vendors a file the release
                                  does not have
  test_pin_not_release_fails      the vendored record pins another commit
  test_malformed_vendor_md_fails  the vendored record names no commit
  test_no_token_fails_loudly      the API mode without SPACE_READ_TOKEN fails
                                  and says so, and never passes or skips
"""

import base64
import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_space_vendor

REPO_ROOT = Path(__file__).resolve().parent.parent

# The five paths the integrating application vendors. The fixture stands in
# for them with two files under sdk, one under sodium and the three single
# files, so a missing, extra or changed file is visible in the count.
RELEASE_FILES = {
    "sdk/rocketnet.h": "/* rocketnet */\n",
    "sdk/rocketnet_api.cpp": "// rocketnet api\n",
    "sodium/sodium.c": "/* sodium */\n",
    "server_xdp/server_xdp.c": "/* server xdp */\n",
    "cmake/EmbedXdp.cmake": "# embed xdp\n",
    "cmake/XdpSources.cmake": "# xdp sources\n",
}

VENDOR_PREFIX = "game/ThirdParty/rocketnet/"


def git(repository, *arguments):
    """Run one git command in a fixture repository and return its output."""
    result = subprocess.run(
        ["git", "-C", str(repository), *arguments],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise AssertionError(f"git {' '.join(arguments)} failed: {result.stderr.strip()}")
    return result.stdout


def write_files(repository, files):
    """Write each name/content pair under the repository root."""
    for name, content in files.items():
        path = Path(repository) / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")


def commit_all(repository, message):
    git(repository, "add", "-A")
    git(
        repository,
        "-c", "user.name=fixture",
        "-c", "user.email=fixture@example.invalid",
        "-c", "commit.gpgsign=false",
        "commit", "-q", "-m", message,
    )
    return git(repository, "rev-parse", "HEAD").strip()


def vendor_record(pin):
    """The vendored record the application writes, with the given pin."""
    return (
        "# Vendored rocketnet\n"
        "\n"
        "| item | value |\n"
        "| --- | --- |\n"
        f"| commit | **{pin}** |\n"
    )


class FakeResponse:
    """The context manager urllib.request.urlopen returns."""

    def __init__(self, payload):
        self.payload = payload

    def read(self):
        return self.payload

    def __enter__(self):
        return self

    def __exit__(self, *arguments):
        return False


class FakeApi:
    """A stand-in for urllib.request.urlopen that answers from canned data."""

    def __init__(self, answers):
        self.answers = answers
        self.requests = []

    def __call__(self, request, timeout=None):
        self.requests.append(request)
        url = request.full_url
        if url not in self.answers:
            raise AssertionError(f"the gate asked for an answer the fixture has none of: {url}")
        return FakeResponse(self.answers[url])

    def tokens(self):
        """The Authorization header of every request the gate made."""
        return [request.get_header("Authorization") for request in self.requests]


def tree_answer(space_repository, space_commit):
    """The canned tree answer for the application repository."""
    entries = []
    for line in git(space_repository, "ls-tree", "-r", space_commit).splitlines():
        meta, _, path = line.partition("\t")
        mode, kind, blob = meta.split()
        entries.append({"path": path, "mode": mode, "type": kind, "sha": blob})
    return json.dumps({"sha": space_commit, "truncated": False, "tree": entries}).encode("utf-8")


def contents_answer(space_repository, space_commit):
    """The canned contents answer for the vendored record."""
    raw = subprocess.run(
        ["git", "-C", str(space_repository), "show", f"{space_commit}:game/ThirdParty/rocketnet/VENDOR.md"],
        check=True,
        capture_output=True,
    ).stdout
    body = json.dumps(
        {"encoding": "base64", "content": base64.b64encode(raw).decode("ascii")}
    )
    return body.encode("utf-8")


def api_answers(space_repository, space_commit, base, repository):
    """Every answer the API mode asks for, keyed by URL."""
    return {
        f"{base}/repos/{repository}/commits/HEAD": json.dumps({"sha": space_commit}).encode("utf-8"),
        f"{base}/repos/{repository}/git/trees/{space_commit}?recursive=1": tree_answer(
            space_repository, space_commit
        ),
        f"{base}/repos/{repository}/contents/game/ThirdParty/rocketnet/VENDOR.md?ref={space_commit}": contents_answer(
            space_repository, space_commit
        ),
    }


class SpaceVendorGateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.release_repository = self.root / "rocketnet"
        self.space_repository = self.root / "space"
        self.release_repository.mkdir()
        self.space_repository.mkdir()

        write_files(self.release_repository, RELEASE_FILES)
        git(self.release_repository, "init", "-q")
        git(self.space_repository, "init", "-q")
        self.release_commit = commit_all(self.release_repository, "release")

    def tearDown(self):
        self.tmp.cleanup()

    def build_space(self, files=None, pin=None, record=None):
        """Build the application fixture and return its commit."""
        vendored = files if files is not None else RELEASE_FILES
        write_files(
            self.space_repository,
            {VENDOR_PREFIX + name: content for name, content in vendored.items()},
        )
        record_path = self.space_repository / "game" / "ThirdParty" / "rocketnet" / "VENDOR.md"
        record_path.parent.mkdir(parents=True, exist_ok=True)
        if record is None:
            record = vendor_record(pin if pin is not None else self.release_commit)
        record_path.write_text(record, encoding="utf-8")
        return commit_all(self.space_repository, "vendor rocketnet")

    def run_gate(self, argv):
        """Run the tool and return (exit code, stdout, stderr)."""
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = check_space_vendor.main(argv)
        return code, out.getvalue(), err.getvalue()

    def test_identical_vendor_passes(self):
        space_commit = self.build_space()

        verdict, code = check_space_vendor.gate(
            self.release_repository,
            self.release_commit,
            check_space_vendor.SpaceClone(self.space_repository),
        )
        self.assertEqual(code, 0, verdict.details)
        self.assertTrue(verdict.ok, verdict.details)
        line = verdict.line()
        self.assertTrue(line.startswith("verdict: pass"), line)
        self.assertIn(f"space_commit={space_commit}", line)
        self.assertIn(f"pin={self.release_commit}", line)
        self.assertIn(f"files={len(RELEASE_FILES)}", line)
        self.assertIn(f"release={self.release_commit}", line)

        # The API mode answers the same question from the same fixture.
        base = check_space_vendor.API_BASE
        answers = api_answers(self.space_repository, space_commit, base, check_space_vendor.SPACE_REPO)
        fake = FakeApi(answers)
        api = check_space_vendor.SpaceApi(
            check_space_vendor.SPACE_REPO, "fixture-token", "HEAD", base, fake
        )
        api_verdict, api_code = check_space_vendor.gate(
            self.release_repository, self.release_commit, api
        )
        self.assertEqual(api_code, 0, api_verdict.details)
        self.assertIn(f"space_commit={space_commit}", api_verdict.line())
        self.assertIn(f"pin={self.release_commit}", api_verdict.line())
        self.assertIn(f"files={len(RELEASE_FILES)}", api_verdict.line())
        self.assertTrue(all(token == "Bearer fixture-token" for token in fake.tokens()), fake.tokens())

        # The tool reads its token from the environment and never prints it.
        with mock.patch.dict(os.environ, {"SPACE_READ_TOKEN": "fixture-token"}):
            with mock.patch.object(check_space_vendor.urllib.request, "urlopen", fake):
                code, out, err = self.run_gate(
                    ["--repo", str(self.release_repository), "--release", self.release_commit]
                )
        self.assertEqual(code, 0, out + err)
        self.assertIn("verdict: pass", out)
        self.assertNotIn("fixture-token", out)
        self.assertNotIn("fixture-token", err)

    def test_changed_byte_fails(self):
        changed = dict(RELEASE_FILES)
        changed["sdk/rocketnet.h"] = "/* rocketnet, edited */\n"
        self.build_space(files=changed)

        verdict, code = check_space_vendor.gate(
            self.release_repository,
            self.release_commit,
            check_space_vendor.SpaceClone(self.space_repository),
        )
        self.assertNotEqual(code, 0)
        self.assertFalse(verdict.ok)
        self.assertIn("reason=changed-file", verdict.line())
        self.assertTrue(any("sdk/rocketnet.h" in detail for detail in verdict.details), verdict.details)

    def test_missing_file_fails(self):
        missing = {name: content for name, content in RELEASE_FILES.items() if name != "sodium/sodium.c"}
        self.build_space(files=missing)

        verdict, code = check_space_vendor.gate(
            self.release_repository,
            self.release_commit,
            check_space_vendor.SpaceClone(self.space_repository),
        )
        self.assertNotEqual(code, 0)
        self.assertIn("reason=missing-file", verdict.line())
        self.assertTrue(any("sodium/sodium.c" in detail for detail in verdict.details), verdict.details)

    def test_extra_file_fails(self):
        extra = dict(RELEASE_FILES)
        extra["sdk/rocketnet_extra.cpp"] = "// not in the release\n"
        self.build_space(files=extra)

        verdict, code = check_space_vendor.gate(
            self.release_repository,
            self.release_commit,
            check_space_vendor.SpaceClone(self.space_repository),
        )
        self.assertNotEqual(code, 0)
        self.assertIn("reason=extra-file", verdict.line())
        self.assertTrue(
            any("sdk/rocketnet_extra.cpp" in detail for detail in verdict.details), verdict.details
        )

    def test_pin_not_release_fails(self):
        other = "0" * 40
        self.build_space(pin=other)

        verdict, code = check_space_vendor.gate(
            self.release_repository,
            self.release_commit,
            check_space_vendor.SpaceClone(self.space_repository),
        )
        self.assertNotEqual(code, 0)
        self.assertIn("reason=pin-mismatch", verdict.line())
        self.assertIn(f"pin={other}", verdict.line())
        self.assertTrue(any(other in detail for detail in verdict.details), verdict.details)

    def test_malformed_vendor_md_fails(self):
        self.build_space(record="# Vendored rocketnet\n\n| commit | **not-a-commit** |\n")

        verdict, code = check_space_vendor.gate(
            self.release_repository,
            self.release_commit,
            check_space_vendor.SpaceClone(self.space_repository),
        )
        self.assertNotEqual(code, 0)
        self.assertIn("reason=malformed-vendor-record", verdict.line())
        self.assertTrue(
            any("VENDOR.md" in detail for detail in verdict.details), verdict.details
        )

    def test_no_token_fails_loudly(self):
        environment = {name: value for name, value in os.environ.items() if name != "SPACE_READ_TOKEN"}
        with mock.patch.dict(os.environ, environment, clear=True):
            code, out, err = self.run_gate(
                ["--repo", str(self.release_repository), "--release", self.release_commit]
            )
        self.assertNotEqual(code, 0)
        self.assertIn("verdict: fail", out)
        self.assertNotIn("verdict: pass", out)
        self.assertNotIn("skip", out.lower())
        self.assertIn("SPACE_READ_TOKEN", out + err)
        self.assertIn("reason=no-token", out)


if __name__ == "__main__":
    unittest.main()
