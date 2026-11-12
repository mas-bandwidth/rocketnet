#!/usr/bin/env python3
"""Only the nightly Application Verifier workflow targets the privileged runner label.

Application Verifier writes HKLM Image File Execution Options, which the fleet's
ordinary Windows runners (NT AUTHORITY\\NETWORK SERVICE) may not; granting that
right to NETWORK SERVICE would let any service or CI job on the host set a
`Debugger` for an image SYSTEM starts. So the verifier runs on a separate runner
instance under an administrator account, with a label of its own (LABEL), which
must only ever run code from this repository's main branch on a schedule or an
owner's dispatch: never code a pull request, a push to a branch or another
workflow chose.

Refused: a workflow or action file that names LABEL anywhere outside a comment,
other than ALLOWED; ALLOWED triggered by anything but `schedule` and
`workflow_dispatch` (no pull_request, pull_request_target, push, workflow_call,
workflow_run, ...); ALLOWED not naming LABEL (the job would fall back to the
ordinary runners and be red there, or the label would have no user); zero
workflow files.

Usage: check_privileged_runner.py [--root DIR] | --self-test
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LABEL = "rocketnet-appverif"
ALLOWED = ".github/workflows/nightly-appverif.yml"
ALLOWED_TRIGGERS = {"schedule", "workflow_dispatch"}
LABEL_WORD = re.compile(r"(?<![A-Za-z0-9_-])" + re.escape(LABEL) + r"(?![A-Za-z0-9_-])")


def scanned_files(root):
    workflows = root / ".github" / "workflows"
    actions = root / ".github" / "actions"
    return (sorted(workflows.glob("*.yml")) + sorted(workflows.glob("*.yaml"))
            + sorted(actions.rglob("*.yml")) + sorted(actions.rglob("*.yaml")))


def strip_comment(line):
    """The line without a trailing YAML comment (a # at the start or after a space, outside quotes)."""
    quote = None
    for index, char in enumerate(line):
        if quote:
            if char == quote:
                quote = None
        elif char in "'\"":
            quote = char
        elif char == "#" and (index == 0 or line[index - 1] in " \t"):
            return line[:index]
    return line


def triggers(text):
    """The trigger names of a workflow's top-level `on:` (inline scalar, inline list or block), or None."""
    lines = text.splitlines()
    for index, raw in enumerate(lines):
        line = strip_comment(raw).rstrip()
        match = re.match(r"^[\"']?on[\"']?\s*:\s*(.*)$", line)
        if not match:
            continue
        value = match.group(1).strip()
        if value.startswith("["):
            return {name.strip().strip("'\"") for name in value.strip("[]").split(",") if name.strip()}
        if value:
            return {value.strip("'\"")}
        names = set()
        indent = None
        for following in lines[index + 1:]:
            body = strip_comment(following).rstrip()
            if not body.strip():
                continue
            if not body.startswith((" ", "\t")):
                break
            width = len(body) - len(body.lstrip())
            if indent is None:
                indent = width
            if width == indent:
                item = body.strip()
                if item.startswith("- "):
                    names.add(item[2:].strip().strip("'\""))
                else:
                    names.add(item.split(":", 1)[0].strip().strip("'\""))
        return names
    return None


def run(root):
    root = Path(root).resolve()
    files = scanned_files(root)
    if not files:
        print(f"check_privileged_runner: 0 workflow files under {root}: nothing was checked")
        return 1
    refused = []
    allowed_names_label = False
    allowed_seen = False
    for path in files:
        relative = path.relative_to(root).as_posix()
        text = path.read_text(encoding="utf-8")
        hits = [number for number, line in enumerate(text.splitlines(), 1) if LABEL_WORD.search(strip_comment(line))]
        if relative == ALLOWED:
            allowed_seen = True
            allowed_names_label = bool(hits)
            found = triggers(text)
            if found is None:
                refused.append(f"{relative}: no `on:` triggers found; it may run only on {sorted(ALLOWED_TRIGGERS)}")
            else:
                for name in sorted(found - ALLOWED_TRIGGERS):
                    refused.append(f"{relative}: triggered by `{name}`: a workflow on the privileged runner label "
                                   f"{LABEL} may run only on {sorted(ALLOWED_TRIGGERS)}, never on code a pull "
                                   "request, a branch push or another workflow chose")
            continue
        for number in hits:
            refused.append(f"{relative}:{number}: names the privileged runner label {LABEL}; only {ALLOWED} may")
    if not allowed_seen:
        refused.append(f"{ALLOWED}: missing; the privileged runner label {LABEL} has no workflow")
    elif not allowed_names_label:
        refused.append(f"{ALLOWED}: does not run on {LABEL}; the ordinary runners may not write Image File Execution Options")
    for line in refused:
        print(line)
    print(f"check_privileged_runner: {len(files)} workflow files, label {LABEL} only in {ALLOWED} "
          f"(schedule and dispatch only): {len(refused)} refused")
    return 1 if refused else 0


GOOD = ("name: nightly-appverif\n\non:\n  schedule:\n    - cron: \"40 7 * * *\"  # nightly\n  workflow_dispatch:\n\n"
        "jobs:\n  a:\n    runs-on: [self-hosted, Windows, X64, rocketnet-appverif]\n    steps:\n      - run: x\n")
OTHER = "on: [push, pull_request]\njobs:\n  b:\n    runs-on: [self-hosted, Windows, X64, rocketnet-windows]\n"


def self_test():
    me = Path(__file__).resolve()
    failures = []

    def case(label, files, want):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name, text in files.items():
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_text(text)
            result = subprocess.run([sys.executable, str(me), "--root", str(root)], capture_output=True, text=True)
            ok = (result.returncode == 0) == (want == 0)
            print(f"{'ok' if ok else 'FAIL'}: {label}: exit {result.returncode}")
            if not ok:
                failures.append(label)
                print(result.stdout + result.stderr)

    good = {ALLOWED: GOOD, ".github/workflows/ci.yml": OTHER}
    case("the nightly alone on the label", good, 0)
    case("a comment naming the label elsewhere", {**good, ".github/workflows/ci.yml": OTHER + "# not rocketnet-appverif\n"}, 0)
    case("a longer label is another label", {**good, ".github/workflows/ci.yml": OTHER.replace("rocketnet-windows", "rocketnet-appverif-2")}, 0)
    case("planted: a pull-request workflow on the label",
         {**good, ".github/workflows/ci.yml": OTHER.replace("rocketnet-windows", "rocketnet-appverif")}, 1)
    case("planted: the label in an expression of another nightly",
         {**good, ".github/workflows/nightly-x.yml": "on:\n  schedule:\n    - cron: '1 1 * * *'\njobs:\n  c:\n"
          "    runs-on: ${{ fromJSON('[\"self-hosted\",\"rocketnet-appverif\"]') }}\n"}, 1)
    case("planted: the label in a composite action",
         {**good, ".github/actions/x/action.yml": "runs:\n  steps:\n    - run: echo rocketnet-appverif\n"}, 1)
    for trigger in ("pull_request", "pull_request_target", "push", "workflow_call", "workflow_run"):
        case(f"planted: the nightly also on {trigger}",
             {**good, ALLOWED: GOOD.replace("  workflow_dispatch:\n", f"  workflow_dispatch:\n  {trigger}:\n")}, 1)
    case("planted: the nightly with an inline pull_request trigger",
         {**good, ALLOWED: GOOD.replace("on:\n  schedule:\n    - cron: \"40 7 * * *\"  # nightly\n  workflow_dispatch:\n",
                                        "on: [schedule, pull_request]\n")}, 1)
    case("planted: the nightly off the label",
         {**good, ALLOWED: GOOD.replace("rocketnet-appverif", "rocketnet-windows")}, 1)
    case("planted: the nightly missing", {".github/workflows/ci.yml": OTHER}, 1)
    case("zero workflow files", {"README": "x\n"}, 1)
    if failures:
        print(f"check_privileged_runner self-test: {len(failures)} failed: {failures}")
        return 1
    print("check_privileged_runner self-test: 15 cases passed")
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=str(ROOT))
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv[1:])
    if args.self_test:
        return self_test()
    return run(args.root)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
