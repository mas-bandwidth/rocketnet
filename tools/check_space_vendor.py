#!/usr/bin/env python3
"""Prove the integrating application vendors this release byte for byte.

The integrating application (robotgamedevelopers/space, private) copies
rocketnet into game/ThirdParty/rocketnet/ with its own re-vendor script. That
script runs `git archive` at a pinned commit over five paths -- sdk, sodium,
server_xdp/server_xdp.c, cmake/EmbedXdp.cmake and cmake/XdpSources.cmake --
and records the pin in game/ThirdParty/rocketnet/VENDOR.md as
`| commit | **<sha40>** |`.

A release of rocketnet is only real if the application that ships it vendors
exactly the commit being released. A release built against a different copy of
the library is a release nobody tested, and the difference is invisible in the
release notes.

The check compares git blob ids, which is byte identity without moving the
content: a blob id is a hash of the file's bytes, so two files with the same
blob id are the same file wherever they live. The release side is read from
this repository at the release commit; the application side is read either
from a local clone of the application (--space-dir) or from the GitHub API
(the default), which needs no clone and no installed tool beyond python.

Two failures are reported, and neither is a skip:

  - the pin in the vendored record is not the release commit: the
    application vendors a different commit than the one being released;
  - the vendored tree is not byte identical to the release tree: a file is
    missing, a file is extra, or a file's bytes differ.

The API mode reads its token from the SPACE_READ_TOKEN environment variable.
The token is never an argument and never printed. A missing token is a
failure that says the secret is not set, never a skip and never a pass: an
unproven release is not a release.

Exit 0 when the application vendors the release commit byte for byte, 1
otherwise. The verdict is one line naming the application commit, the pin it
records and the number of files compared.
"""

import argparse
import base64
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import urllib.error
import urllib.request

REPO_ROOT = Path(__file__).resolve().parent.parent

# Where the application keeps the vendored library, and the record it writes
# beside it. The record is the application's own file and is not part of the
# comparison.
VENDOR_ROOT = "game/ThirdParty/rocketnet"
VENDOR_RECORD = "VENDOR.md"

# The paths the application vendors. The comparison covers every file under
# them, not these five names, because the release side is read with
# `git ls-tree -r`, which recurses.
RELEASE_PATHS = (
    "sdk",
    "sodium",
    "server_xdp/server_xdp.c",
    "cmake/EmbedXdp.cmake",
    "cmake/XdpSources.cmake",
)

SPACE_REPO = "robotgamedevelopers/space"
TOKEN_ENV = "SPACE_READ_TOKEN"
API_BASE = "https://api.github.com"

# The pin row. The bold decoration is optional; the commit must be forty
# hexadecimal characters.
COMMIT_ROW = re.compile(
    r"^[ \t]*\|[ \t]*commit[ \t]*\|[ \t]*"
    r"(?:\*\*([0-9a-fA-F]{40})\*\*|([0-9a-fA-F]{40}))"
    r"[ \t]*\|[ \t]*$",
    re.MULTILINE,
)

SHA = re.compile(r"[0-9a-f]{40}")


class GateError(Exception):
    """A failure that ends the gate before it can reach a verdict."""


class MissingToken(GateError):
    """The API mode has no token to read the application with."""


class MalformedRecord(GateError):
    """The vendored record names no commit."""


REASON = {
    MissingToken: "no-token",
    MalformedRecord: "malformed-vendor-record",
}


class Verdict:
    """The outcome of the gate, and the one line that says it."""

    def __init__(self):
        self.ok = False
        self.reason = ""
        self.space_commit = "-"
        self.pin = "-"
        self.files = "-"
        self.release = "-"
        self.details = []

    def line(self):
        text = (
            f"verdict: {'pass' if self.ok else 'fail'} "
            f"space_commit={self.space_commit} pin={self.pin} "
            f"files={self.files} release={self.release}"
        )
        if not self.ok:
            text += f" reason={self.reason}"
        return text


def git(repository, *arguments):
    """Run one git command in a repository and return its output as text."""
    result = subprocess.run(
        ["git", "-C", str(repository), *arguments],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        detail = result.stderr.strip().splitlines()
        raise GateError(
            f"git {' '.join(arguments)} failed in {repository}: "
            f"{detail[0] if detail else 'no message'}"
        )
    return result.stdout


def git_bytes(repository, *arguments):
    """Run one git command in a repository and return its output as bytes."""
    result = subprocess.run(
        ["git", "-C", str(repository), *arguments],
        check=False,
        capture_output=True,
    )
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", "replace").strip().splitlines()
        raise GateError(
            f"git {' '.join(arguments)} failed in {repository}: "
            f"{detail[0] if detail else 'no message'}"
        )
    return result.stdout


def parse_tree(text):
    """Read `git ls-tree -r` output into {path: blob id}."""
    files = {}
    for line in text.splitlines():
        meta, _, path = line.partition("\t")
        fields = meta.split()
        if len(fields) != 3 or not path:
            continue
        _mode, kind, blob = fields
        if kind != "blob":
            continue
        files[path] = blob
    return files


def release_commit(repository, release):
    """The commit the release names, as forty hexadecimal characters."""
    sha = git(repository, "rev-parse", "--verify", f"{release}^{{commit}}").strip()
    if not SHA.fullmatch(sha):
        raise GateError(f"{release} is not a commit in {repository}")
    return sha


def release_tree(repository, commit):
    """The blob ids of the released files, keyed by their path."""
    text = git(repository, "ls-tree", "-r", commit, "--", *RELEASE_PATHS)
    files = parse_tree(text)
    if not files:
        raise GateError(
            f"the release commit {commit} carries none of the vendored paths: "
            f"{', '.join(RELEASE_PATHS)}"
        )
    return files


def parse_pin(record):
    """The commit the vendored record pins, or a MalformedRecord."""
    pins = set()
    for match in COMMIT_ROW.finditer(record):
        pins.add((match.group(1) or match.group(2)).lower())
    if not pins:
        raise MalformedRecord(
            f"{VENDOR_ROOT}/{VENDOR_RECORD} names no commit: the record must carry a row "
            "of the form | commit | **<40 hexadecimal characters>** |"
        )
    if len(pins) > 1:
        raise MalformedRecord(
            f"{VENDOR_ROOT}/{VENDOR_RECORD} names more than one commit: "
            f"{', '.join(sorted(pins))}"
        )
    return pins.pop()


class SpaceClone:
    """The application's tree, read from a local clone with git."""

    def __init__(self, directory, ref="HEAD"):
        self.directory = Path(directory)
        self.ref = ref

    def space_commit(self):
        sha = git(self.directory, "rev-parse", "--verify", f"{self.ref}^{{commit}}").strip()
        if not SHA.fullmatch(sha):
            raise GateError(f"{self.ref} is not a commit in {self.directory}")
        return sha

    def vendor_files(self):
        text = git(self.directory, "ls-tree", "-r", self.ref, "--", VENDOR_ROOT)
        prefix = VENDOR_ROOT + "/"
        files = {}
        for path, blob in parse_tree(text).items():
            if not path.startswith(prefix):
                continue
            relative = path[len(prefix):]
            if relative == VENDOR_RECORD:
                continue
            files[relative] = blob
        if not files:
            raise GateError(
                f"{self.directory} has no vendored rocketnet tree under {VENDOR_ROOT}"
            )
        return files

    def vendor_record(self):
        raw = git_bytes(self.directory, "show", f"{self.ref}:{VENDOR_ROOT}/{VENDOR_RECORD}")
        return raw.decode("utf-8", "replace")


class SpaceApi:
    """The application's tree, read through the GitHub API."""

    def __init__(self, repository=SPACE_REPO, token=None, ref="HEAD", base=API_BASE, opener=None):
        self.repository = repository
        self.token = token
        self.ref = ref
        self.base = base.rstrip("/")
        self.opener = opener or urllib.request.urlopen
        self._commit = None

    def _request(self, path, accept):
        url = f"{self.base}{path}"
        request = urllib.request.Request(url)
        request.add_header("Accept", accept)
        request.add_header("Authorization", f"Bearer {self.token}")
        request.add_header("User-Agent", "rocketnet-space-vendor-gate")
        try:
            with self.opener(request, timeout=60) as response:
                return response.read()
        except urllib.error.HTTPError as error:
            raise GateError(f"GitHub answered {error.code} for {url}") from None
        except urllib.error.URLError as error:
            raise GateError(f"GitHub was unreachable for {url}: {error.reason}") from None

    def _json(self, path, accept="application/vnd.github+json"):
        raw = self._request(path, accept)
        try:
            return json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise GateError(f"GitHub answered {path} with something that is not JSON: {error}") from None

    def space_commit(self):
        if self._commit is None:
            payload = self._json(f"/repos/{self.repository}/commits/{self.ref}")
            sha = payload.get("sha", "")
            if not isinstance(sha, str) or not SHA.fullmatch(sha):
                raise GateError(f"GitHub named no commit for {self.ref} in {self.repository}")
            self._commit = sha
        return self._commit

    def vendor_files(self):
        commit = self.space_commit()
        payload = self._json(f"/repos/{self.repository}/git/trees/{commit}?recursive=1")
        if payload.get("truncated"):
            raise GateError(
                f"the tree of {self.repository} at {commit} is too large for one answer; "
                "the gate cannot prove it whole"
            )
        prefix = VENDOR_ROOT + "/"
        files = {}
        for entry in payload.get("tree", []):
            if not isinstance(entry, dict) or entry.get("type") != "blob":
                continue
            path = entry.get("path", "")
            if not path.startswith(prefix):
                continue
            relative = path[len(prefix):]
            if relative == VENDOR_RECORD:
                continue
            blob = entry.get("sha", "")
            if not isinstance(blob, str) or not SHA.fullmatch(blob):
                raise GateError(f"the tree of {self.repository} names no blob id for {path}")
            files[relative] = blob
        if not files:
            raise GateError(
                f"the tree of {self.repository} has no vendored rocketnet tree under {VENDOR_ROOT}"
            )
        return files

    def vendor_record(self):
        # The contents are asked for at the resolved commit, not at the ref
        # name: a commit names one tree, and a ref name can move under the
        # answer.
        payload = self._json(
            f"/repos/{self.repository}/contents/{VENDOR_ROOT}/{VENDOR_RECORD}?ref={self.space_commit()}"
        )
        content = payload.get("content", "")
        if payload.get("encoding") != "base64" or not isinstance(content, str):
            raise GateError(
                f"GitHub answered the contents of {VENDOR_ROOT}/{VENDOR_RECORD} with "
                "no base64 body"
            )
        try:
            raw = base64.b64decode(content)
        except (ValueError, TypeError) as error:
            raise GateError(
                f"the contents of {VENDOR_ROOT}/{VENDOR_RECORD} are not base64: {error}"
            ) from None
        return raw.decode("utf-8", "replace")


def gate(repository, release, source):
    """Run the gate and return (verdict, exit code)."""
    verdict = Verdict()
    try:
        verdict.release = release_commit(repository, release)
        verdict.space_commit = source.space_commit()
        wanted = release_tree(repository, verdict.release)
        have = source.vendor_files()
        verdict.files = len(wanted)
        record = source.vendor_record()
    except GateError as error:
        verdict.reason = REASON.get(type(error), "gate-error")
        verdict.details.append(str(error))
        return verdict, 1

    try:
        verdict.pin = parse_pin(record)
    except MalformedRecord as error:
        verdict.reason = "malformed-vendor-record"
        verdict.details.append(str(error))
        return verdict, 1

    missing = sorted(set(wanted) - set(have))
    extra = sorted(set(have) - set(wanted))
    changed = sorted(
        path for path in set(wanted) & set(have) if wanted[path] != have[path]
    )

    if verdict.pin != verdict.release:
        verdict.details.append(
            f"the application pins {verdict.pin} but the release commit is {verdict.release}: "
            "re-vendor the application at the release commit"
        )
    for path in missing:
        verdict.details.append(f"the application does not vendor {path}")
    for path in extra:
        verdict.details.append(f"the application vendors {path}, which the release does not have")
    for path in changed:
        verdict.details.append(f"the application vendors {path} with different bytes")

    if verdict.pin != verdict.release:
        verdict.reason = "pin-mismatch"
    elif missing:
        verdict.reason = "missing-file"
    elif extra:
        verdict.reason = "extra-file"
    elif changed:
        verdict.reason = "changed-file"
    else:
        verdict.ok = True

    return verdict, 0 if verdict.ok else 1


def space_source(args):
    """The application side: a local clone, or the API with its token."""
    if args.space_dir is not None:
        return SpaceClone(args.space_dir, args.space_ref)
    token = os.environ.get(TOKEN_ENV, "")
    if not token.strip():
        raise MissingToken(
            f"{TOKEN_ENV} is not set: the gate reads {args.space_repo} with that token and "
            "will not report a release as proven without it"
        )
    return SpaceApi(args.space_repo, token.strip(), args.space_ref, args.api_base)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--release",
        default="HEAD",
        help="the rocketnet commit being released (default: HEAD)",
    )
    parser.add_argument(
        "--repo",
        type=Path,
        default=REPO_ROOT,
        help="the rocketnet checkout the release commit is read from (default: this checkout)",
    )
    parser.add_argument(
        "--space-dir",
        type=Path,
        help="a local clone of the integrating application; without it the application is read through the API",
    )
    parser.add_argument(
        "--space-ref",
        default="HEAD",
        help="the application commit to read (default: HEAD)",
    )
    parser.add_argument(
        "--space-repo",
        default=SPACE_REPO,
        help=f"the integrating application repository (default: {SPACE_REPO})",
    )
    parser.add_argument(
        "--api-base",
        default=API_BASE,
        help=f"the GitHub API base URL (default: {API_BASE})",
    )
    args = parser.parse_args(argv)

    verdict = Verdict()
    code = 1
    try:
        verdict.release = release_commit(args.repo, args.release)
    except GateError as error:
        verdict.reason = "gate-error"
        verdict.details.append(str(error))
        print(verdict.line())
        sys.stdout.flush()
        for detail in verdict.details:
            print(detail, file=sys.stderr)
        return 1

    try:
        source = space_source(args)
        verdict, code = gate(args.repo, args.release, source)
    except GateError as error:
        verdict.reason = REASON.get(type(error), "gate-error")
        verdict.details.append(str(error))

    print(verdict.line())
    sys.stdout.flush()
    for detail in verdict.details:
        print(detail, file=sys.stderr)
    return code


if __name__ == "__main__":
    sys.exit(main())
