#!/usr/bin/env python3
"""Check the platform policy table against the workflows it names.

Policy: docs/PLATFORMS.md, "The table" and "Policy rules". A support claim is
only a claim when a workflow file names the runner that proves it. This script
reads the markdown table directly; it does not parse YAML.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOCUMENT = ROOT / "docs" / "PLATFORMS.md"

TIERS = {"supported", "nightly", "unsupported"}
COLUMNS = {
    "operating system": "os",
    "architecture": "arch",
    "compiler": "compiler",
    "drivers": "drivers",
    "tier": "tier",
    "workflow": "workflow",
    "runner label": "label",
}


def normalise_header(cell):
    """Reduce a header cell to the words used by COLUMNS.

    Policy: docs/PLATFORMS.md, "The table".
    """
    return re.sub(r"[^a-z0-9]+", " ", cell.strip().lower()).strip()


def split_row(line):
    """Split one markdown table row into stripped cells.

    Policy: docs/PLATFORMS.md, "The table".
    """
    return [cell.strip() for cell in line.strip().strip("|").split("|")]


def clean(cell):
    """Remove markdown code ticks and surrounding whitespace from a value.

    Policy: docs/PLATFORMS.md, "The table".
    """
    return cell.strip().strip("`").strip()


def is_placeholder(value):
    """True for the table's way of saying "no value".

    Policy: docs/PLATFORMS.md, "A claim with no leg is refused".
    """
    return value in ("", "-", "\u2014", "n/a", "none")


def parse_table(text):
    """Return every row of the policy table as a dict keyed by COLUMNS.

    Policy: docs/PLATFORMS.md, "The table". The first header line that names
    every required column starts the table; rows continue until a line that
    is not a markdown row.
    """
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if not line.lstrip().startswith("|"):
            continue
        headers = [normalise_header(cell) for cell in split_row(line)]
        keys = [COLUMNS.get(header) for header in headers]
        if any(key is None for key in keys):
            continue
        if index + 1 >= len(lines) or not lines[index + 1].lstrip().startswith("|"):
            continue
        rows = []
        for row_line in lines[index + 2:]:
            if not row_line.lstrip().startswith("|"):
                break
            cells = split_row(row_line)
            if len(cells) != len(keys):
                raise ValueError(
                    f"row has {len(cells)} cells, header has {len(keys)}: {row_line.strip()}"
                )
            rows.append({key: clean(cell) for key, cell in zip(keys, cells)})
        return rows
    raise ValueError("no platform policy table found")


def label_tokens(label):
    """Split a runner label into the tokens a workflow must contain.

    Policy: docs/PLATFORMS.md, "A claim with no leg is refused". Labels are
    written as comma-separated YAML lists or as a single image name, so a
    token is any comma- or whitespace-separated part with quotes, brackets
    and code ticks removed.
    """
    tokens = []
    for token in re.split(r"[,\s]+", label):
        token = token.strip("[]\"'`")
        if token:
            tokens.append(token)
    return tokens


def check_row(row, workflow_cache):
    """Return the failures for one table row.

    Policy: docs/PLATFORMS.md, "Policy rules": a supported or nightly row must
    name a workflow file and a runner label, the file must exist, and every
    token of the label must occur in that file. An unsupported row names no
    leg.
    """
    name = f"{row['os']} / {row['arch']} / {row['compiler']}"
    tier = row["tier"].lower()
    workflow = row["workflow"]
    label = row["label"]

    if tier not in TIERS:
        return [f"{name}: tier {row['tier']!r} is not one of supported, nightly, unsupported"]

    if tier == "unsupported":
        if not is_placeholder(workflow) or not is_placeholder(label):
            return [f"{name}: unsupported row names a workflow or runner label"]
        return []

    if is_placeholder(workflow) or is_placeholder(label):
        return [
            f"{name}: {tier} claim has no CI leg "
            f"(workflow={workflow!r}, runner label={label!r})"
        ]

    path = ROOT / workflow
    if not path.is_file():
        return [f"{name}: workflow file {workflow!r} does not exist"]

    text = workflow_cache.get(path)
    if text is None:
        text = path.read_text(encoding="utf-8")
        workflow_cache[path] = text

    missing = [token for token in label_tokens(label) if token not in text]
    if missing:
        return [
            f"{name}: runner label token {token!r} from {label!r} "
            f"does not appear in {workflow}"
            for token in missing
        ]
    return []


def main():
    """Check every row and exit 0 only when the whole policy holds.

    Policy: docs/PLATFORMS.md, "Checking the table".
    """
    try:
        rows = parse_table(DOCUMENT.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        print(f"check_platform_policy: {error}", file=sys.stderr)
        return 1

    workflow_cache = {}
    failures = []
    for row in rows:
        failures.extend(check_row(row, workflow_cache))

    if failures:
        for failure in failures:
            print(f"check_platform_policy: {failure}", file=sys.stderr)
        return 1

    supported = sum(1 for row in rows if row["tier"].lower() == "supported")
    nightly = sum(1 for row in rows if row["tier"].lower() == "nightly")
    unsupported = len(rows) - supported - nightly
    print(
        f"check_platform_policy: {supported} supported, {nightly} nightly, "
        f"{unsupported} unsupported row(s) match their named workflows"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
