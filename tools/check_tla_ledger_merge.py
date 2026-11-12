#!/usr/bin/env python3
"""The four TLA ledgers merge clean when two TLA cards each append rows.

The four ledgers (proofs/tla/CASES.tsv, proofs/tla/COVERAGE.tsv, proofs/tla/RUNS.tsv and
proofs/tla/README.md) merge with git's union transport, so two TLA cards that append
rows land without conflict. An edit to an existing row next to another
branch's append comes out doubled, refused by tlacheck for CASES.tsv and
RUNS.tsv and by tools/check_tla_coverage.py for COVERAGE.tsv; a doubled
README line is removed by hand. This script stages a two-branch merge of an
append of each ledger and proves both names survive.
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LEDGERS = [
    "proofs/tla/CASES.tsv",
    "proofs/tla/COVERAGE.tsv",
    "proofs/tla/RUNS.tsv",
    "proofs/tla/README.md",
]


def git(repo, *args):
    return subprocess.run(
        ["git", "-c", "user.name=ledger-check", "-c",
         "user.email=ledger-check@invalid"] + list(args),
        cwd=repo, capture_output=True, text=True,
    )


def append_tsv_row(path, first, fill):
    lines = path.read_text().splitlines()
    header = lines[0].split("\t")
    row = [fill] * len(header)
    row[0] = first
    lines.append("\t".join(row))
    path.write_text("\n".join(lines) + "\n")


def append_readme_row(path, name):
    lines = path.read_text().splitlines()
    last = max(i for i, line in enumerate(lines) if line.startswith("| `"))
    lines.insert(last + 1, f"| `{name}` | 1 | {name} |")
    path.write_text("\n".join(lines) + "\n")


def commit_branch(repo, name, ledger):
    for rel in LEDGERS:
        path = repo / rel
        if rel.endswith(".tsv"):
            append_tsv_row(path, f"MCLedger{ledger}.cfg", f"Ledger{ledger}")
        else:
            append_readme_row(path, f"Ledger{ledger}")
    git(repo, "add", ".")
    git(repo, "commit", "-q", "-m", name)


def run(root=ROOT):
    failures = []
    present = [rel for rel in LEDGERS + [".gitattributes"] if (root / rel).is_file()]
    if len(present) != len(LEDGERS) + 1:
        missing = sorted(set(LEDGERS + [".gitattributes"]) - set(present))
        print(f"LEDGER MERGE FAIL {len(present)} files of {len(LEDGERS) + 1} under {root}: missing {', '.join(missing)}")
        return 1
    build = root / "build"
    build.mkdir(exist_ok=True)
    scratch = Path(tempfile.mkdtemp(prefix="tla-ledger-merge.", dir=str(build)))
    try:
        git(scratch, "init", "-q", "-b", "main")
        for rel in LEDGERS + [".gitattributes"]:
            dst = scratch / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(root / rel, dst)
        git(scratch, "add", ".")
        git(scratch, "commit", "-q", "-m", "base")

        git(scratch, "checkout", "-q", "-b", "a")
        commit_branch(scratch, "a", "A")
        git(scratch, "checkout", "-q", "main")
        git(scratch, "checkout", "-q", "-b", "b")
        commit_branch(scratch, "b", "B")
        git(scratch, "checkout", "-q", "main")

        for branch in ("a", "b"):
            proc = git(scratch, "merge", "--no-ff", "--no-edit", branch)
            if proc.returncode != 0:
                out = (proc.stdout or "") + (proc.stderr or "")
                for line in out.splitlines():
                    if "CONFLICT" in line:
                        failures.append(line.strip())
                git(scratch, "merge", "--abort")

        for rel in LEDGERS:
            text = (scratch / rel).read_text()
            if "LedgerA" not in text or "LedgerB" not in text:
                failures.append(f"{rel}: does not contain both LedgerA and LedgerB")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)

    if failures:
        for failure in failures:
            print(failure)
        print(f"LEDGER MERGE FAIL {len(failures)}")
        return 1
    print("LEDGER MERGE OK 4 ledgers, 2 branches")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT, help="tree holding the ledgers and .gitattributes (default: this repository)")
    sys.exit(run(parser.parse_args().root.resolve()))
