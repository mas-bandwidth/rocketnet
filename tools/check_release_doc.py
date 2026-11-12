#!/usr/bin/env python3
"""Check that the release document names every version site.

The contract comes from the `version-consistency` job in
.github/workflows/release-check.yml, the release design in this repository: the
`version sites` are the files whose contents that job reads and compares with
each other and with the release tag. This script extracts those file names from
that job with a regular expression and checks each one appears in
docs/RELEASING.md.

Exit 0 when every site is named, 1 when one is missing (named on stderr).
"""

import argparse
from pathlib import Path
import re
import sys

REPO_ROOT = Path(__file__).resolve().parent.parent
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "release-check.yml"
RELEASING_DOC = REPO_ROOT / "docs" / "RELEASING.md"

JOB_NAME = "version-consistency"
JOB_HEADER = re.compile(r"^  ([A-Za-z0-9_.-]+):\s*$")

# A file name with an extension, optionally preceded by path components. The
# lookarounds keep it from matching fragments of longer words or URLs.
FILE_NAME = re.compile(
    r"(?<![\w./-])"
    r"(?:[A-Za-z0-9_.-]+/)*"
    r"[A-Za-z0-9_.-]+\.(?:h|hh|hpp|hxx|c|cc|cpp|cxx|py|txt|md|json|ya?ml|cmake)"
    r"(?![\w./-])"
)


def job_block(text, name):
    """Return the lines of one job, from its key to the next job key."""
    lines = text.splitlines()
    start = None
    for index, line in enumerate(lines):
        match = JOB_HEADER.match(line)
        if match and match.group(1) == name:
            start = index
            break
    if start is None:
        return None
    end = len(lines)
    for index in range(start + 1, len(lines)):
        if JOB_HEADER.match(lines[index]):
            end = index
            break
    return "\n".join(lines[start:end])


def version_sites(text):
    """Return the unique version-site file names the job reads, in file order."""
    block = job_block(text, JOB_NAME)
    if block is None:
        raise ValueError(f".github/workflows/release-check.yml has no {JOB_NAME} job")
    sites = []
    for match in FILE_NAME.finditer(block):
        site = match.group(0)
        if site not in sites:
            sites.append(site)
    return sites


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=REPO_ROOT,
                        help="the repository root to check (default: this repository)")
    args = parser.parse_args(argv)
    workflow = args.root / ".github" / "workflows" / "release-check.yml"
    releasing_doc = args.root / "docs" / "RELEASING.md"
    try:
        workflow_text = workflow.read_text(encoding="utf-8")
        doc_text = releasing_doc.read_text(encoding="utf-8")
    except OSError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    try:
        sites = version_sites(workflow_text)
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    if not sites:
        print(
            f"error: .github/workflows/release-check.yml {JOB_NAME} job names "
            "0 version sites",
            file=sys.stderr,
        )
        return 1

    missing = [site for site in sites if site not in doc_text]
    if missing:
        for site in missing:
            print(
                f"docs/RELEASING.md does not name version site: {site}",
                file=sys.stderr,
            )
        return 1

    print(
        "ok: docs/RELEASING.md names "
        f"{len(sites)} version site(s): {', '.join(sites)}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
