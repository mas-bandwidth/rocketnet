#!/usr/bin/env python3
"""Every scheduled workflow is watched, and (with --remote) every cron fires from main.

Not covered: whether a scheduled run passed (that is the watch itself,
tools/check_nightly_dispositions.py), and the watch's own workflow, which is
the reader and is not in its own list (nightly-watch.yml; its red is its own
run's conclusion).

Local mode (the lint): the set of workflow files whose `on:` has a
`schedule:` with at least one cron, read here with tools/workflow_jobs.py,
must EQUAL the set `tools/check_nightly_dispositions.py --list` prints (the
watch's own selection), the watch excepted. A scheduled file the watch does
not read, a listed file with no schedule, a `schedule:` with no cron, and
zero scheduled workflows are refused.

--remote mode (not in the lint: the nightly planted-violations job and by
hand; needs GITHUB_TOKEN and --repo or GITHUB_REPOSITORY): reads the
repository's registered workflows (GET /repos/R/actions/workflows) and the
files in .github/workflows on main (GET /repos/R/contents/.github/workflows
?ref=main). Refused: a registered, active workflow whose file is not on main
and that carries a cron on the branch of its latest run (GitHub fires crons
from the default branch only, so it never runs: VERIFY-2 B4); one whose
file or branch cannot be read (unknown is red); a scheduled file on main
that is not registered or not active (disabled: it never fires). Every GET
goes through an injectable fetch, so the self-test runs on fixtures.

Usage: check_scheduled_watched.py [--root DIR]
       check_scheduled_watched.py --remote [--repo OWNER/NAME]
       check_scheduled_watched.py --self-test
"""

import argparse
import base64
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import workflow_jobs  # noqa: E402

ROOT = HERE.parent
WATCH_FILE = "nightly-watch.yml"
WORKFLOWS = ".github/workflows"


def crons_of(text):
    """The cron strings of a workflow's schedule trigger: None when it has no
    schedule trigger, [] when it has one with no cron."""
    on = workflow_jobs.parse(text).get("on")
    if not isinstance(on, dict) or "schedule" not in on:
        return None
    schedule = on.get("schedule") or []
    return [row.get("cron") for row in schedule if isinstance(row, dict) and row.get("cron")]


def scheduled_files(root):
    """({file: [cron]}, problems) for .github/workflows under root."""
    directory = Path(root) / WORKFLOWS
    found, problems = {}, []
    files = sorted(directory.glob("*.yml")) + sorted(directory.glob("*.yaml"))
    for path in files:
        try:
            crons = crons_of(path.read_text(encoding="utf-8"))
        except workflow_jobs.WorkflowError as error:
            problems.append(f"{path.name}: cannot be read: {error}")
            continue
        if crons is None:
            continue
        if not crons:
            problems.append(f"{path.name}: `schedule:` with no cron: it never fires")
            continue
        found[path.name] = crons
    return files, found, problems


def watch_list(root):
    result = subprocess.run([sys.executable, str(Path(root) / "tools" / "check_nightly_dispositions.py"),
                             "--list", "--root", str(root)], capture_output=True, text=True)
    if result.returncode != 0:
        return None, f"the watch's --list exited {result.returncode}: {(result.stdout + result.stderr).strip()}"
    return {line.strip() for line in result.stdout.splitlines() if line.strip()}, None


def local(root):
    root = Path(root).resolve()
    files, scheduled, problems = scheduled_files(root)
    if not files:
        print(f"check_scheduled_watched: 0 workflow files under {root / WORKFLOWS}: nothing was checked")
        return 1
    expected = set(scheduled) - {WATCH_FILE}
    if not expected:
        problems.append("0 scheduled workflows (the watch excepted): nothing is watched")
    listed, error = watch_list(root)
    if error:
        problems.append(error)
    else:
        for name in sorted(expected - listed):
            problems.append(f"{name}: scheduled ({', '.join(scheduled[name])}) and not in the watch's list")
        for name in sorted(listed - expected):
            problems.append(f"{name}: in the watch's list and not a scheduled workflow here")
    for problem in problems:
        print(f"SCHEDULED-UNWATCHED {problem}")
    print(f"check_scheduled_watched: {len(files)} workflow files, {len(expected)} scheduled (the watch "
          f"excepted), {len(listed or ())} in the watch's list, {len(problems)} problems")
    return 1 if problems else 0


# ---------------------------------------------------------------- remote


def gh_fetch(path):
    """GET a GitHub API path with GITHUB_TOKEN (the watch's own fetch)."""
    import check_nightly_dispositions as watch
    return watch.http_fetch(watch.API + path)


def remote(repo, fetch):
    problems = []
    try:
        registered = fetch(f"/repos/{repo}/actions/workflows?per_page=100").get("workflows", [])
        listing = fetch(f"/repos/{repo}/contents/{WORKFLOWS}?ref=main")
    except Exception as error:  # noqa: BLE001 -- an unreadable API is red, named
        print(f"SCHEDULED-REMOTE API-ERROR {error}")
        return 1
    on_main = {entry["path"] for entry in listing if entry.get("type") == "file"}
    if not registered or not on_main:
        print(f"SCHEDULED-REMOTE {len(registered)} registered workflows, {len(on_main)} files on main: "
              "nothing was checked")
        return 1
    by_path = {w["path"]: w for w in registered}
    for workflow in registered:
        path = workflow["path"]
        if not path.startswith(WORKFLOWS + "/") or path in on_main or workflow.get("state") != "active":
            continue
        try:
            runs = fetch(f"/repos/{repo}/actions/workflows/{workflow['id']}/runs?per_page=1").get("workflow_runs", [])
            if not runs:
                problems.append(f"{path}: registered, not on main, and no run names its branch: cannot judge")
                continue
            branch = runs[0]["head_branch"]
            sha = runs[0].get("head_sha") or branch
            content = fetch(f"/repos/{repo}/contents/{path}?ref={sha}")
            text = base64.b64decode(content["content"]).decode("utf-8")
            crons = crons_of(text)
        except Exception as error:  # noqa: BLE001
            problems.append(f"{path}: registered, not on main, unreadable: {error}")
            continue
        if crons:
            problems.append(f"{path}: cron {', '.join(crons)} on branch {branch} and not on main: it never fires")
    for path in sorted(on_main):
        try:
            content = fetch(f"/repos/{repo}/contents/{path}?ref=main")
            crons = crons_of(base64.b64decode(content["content"]).decode("utf-8"))
        except Exception as error:  # noqa: BLE001
            problems.append(f"{path}: on main, unreadable: {error}")
            continue
        if not crons:
            continue
        workflow = by_path.get(path)
        if workflow is None:
            problems.append(f"{path}: scheduled on main and not registered: it has never fired")
        elif workflow.get("state") != "active":
            problems.append(f"{path}: scheduled on main and {workflow.get('state')}: it never fires")
    for problem in problems:
        print(f"SCHEDULED-REMOTE {problem}")
    print(f"check_scheduled_watched --remote: {len(registered)} registered, {len(on_main)} files on main, "
          f"{len(problems)} problems")
    return 1 if problems else 0


# ---------------------------------------------------------------- self-test


def self_test():
    failures = []

    def expect(label, got, want):
        ok = (got == 0) == (want == 0)
        print(f"{'ok' if ok else 'FAIL'}: {label}: exit {got}")
        if not ok:
            failures.append(label)

    def workflow(cron):
        return (f"on:\n  schedule:\n    - cron: '{cron}'\n" if cron else "on:\n  push:\n") + \
            "jobs:\n  a:\n    runs-on: [self-hosted]\n    steps:\n      - run: make\n"

    # local: a fixture root with the real watch beside it, so --list is the watch's own
    def local_case(label, files, want):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / WORKFLOWS).mkdir(parents=True)
            (root / "tools").mkdir()
            for name in ("check_nightly_dispositions.py", "workflow_jobs.py"):
                (root / "tools" / name).write_text((HERE / name).read_text())
            for name, text in files.items():
                (root / WORKFLOWS / name).write_text(text)
            result = subprocess.run([sys.executable, str(Path(__file__).resolve()), "--root", str(root)],
                                    capture_output=True, text=True)
            expect(label, result.returncode, want)
            if (result.returncode == 0) != (want == 0):
                print(result.stdout + result.stderr)

    watch = {WATCH_FILE: workflow("0 12 * * *")}
    local_case("two scheduled, both listed", {**watch, "a.yml": workflow("0 1 * * *"),
                                              "b.yml": workflow("0 2 * * *"), "c.yml": workflow(None)}, 0)
    local_case("a schedule with no cron", {**watch, "a.yml": workflow("0 1 * * *"),
                                           "b.yml": "on:\n  schedule: []\njobs: {}\n"}, 1)
    local_case("zero scheduled workflows", {**watch, "c.yml": workflow(None)}, 1)
    local_case("zero workflow files", {}, 1)

    # the watch's selection drifting from the schedule triggers: a watch whose
    # --list omits one file (planted by a fake watch)
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / WORKFLOWS).mkdir(parents=True)
        (root / "tools").mkdir()
        (root / "tools" / "workflow_jobs.py").write_text((HERE / "workflow_jobs.py").read_text())
        (root / "tools" / "check_nightly_dispositions.py").write_text("print('a.yml')\n")
        for name, text in {"a.yml": workflow("0 1 * * *"), "b.yml": workflow("0 2 * * *")}.items():
            (root / WORKFLOWS / name).write_text(text)
        result = subprocess.run([sys.executable, str(Path(__file__).resolve()), "--root", str(root)],
                                capture_output=True, text=True)
        expect("a scheduled file the watch does not list", result.returncode, 1)
        if "b.yml: scheduled" not in result.stdout:
            failures.append("unwatched file not named")
            print(result.stdout)

    # remote, on fixtures
    def b64(text):
        return {"content": base64.b64encode(text.encode()).decode()}

    def fixture(registered, main_files, branch_files=None, runs=None):
        def fetch(path):
            if path.startswith("/repos/o/r/actions/workflows?"):
                return {"workflows": registered}
            if path == f"/repos/o/r/contents/{WORKFLOWS}?ref=main":
                return [{"type": "file", "path": p} for p in main_files]
            if "/runs?" in path:
                wid = int(path.split("/workflows/")[1].split("/")[0])
                return {"workflow_runs": (runs or {}).get(wid, [])}
            if "/contents/" in path:
                p, ref = path.split("/contents/")[1].split("?ref=")
                source = main_files if ref == "main" else (branch_files or {})
                if p not in source:
                    raise OSError(f"404 {p}@{ref}")
                return b64(source[p])
            raise OSError(f"unexpected {path}")
        return fetch

    def reg(wid, name, state="active"):
        return {"id": wid, "path": f"{WORKFLOWS}/{name}", "state": state}

    main = {f"{WORKFLOWS}/a.yml": workflow("0 1 * * *"), f"{WORKFLOWS}/c.yml": workflow(None)}
    expect("remote: every cron registered and on main",
           remote("o/r", fixture([reg(1, "a.yml"), reg(3, "c.yml")], main)), 0)
    expect("remote: a cron registered from a branch, not on main",
           remote("o/r", fixture([reg(1, "a.yml"), reg(2, "b.yml")], main,
                                 {f"{WORKFLOWS}/b.yml": workflow("0 3 * * *")},
                                 {2: [{"head_branch": "ci/x", "head_sha": "ci/x"}]})), 1)
    expect("remote: a non-scheduled workflow on a branch is fine",
           remote("o/r", fixture([reg(1, "a.yml"), reg(2, "b.yml")], main,
                                 {f"{WORKFLOWS}/b.yml": workflow(None)}, {2: [{"head_branch": "ci/x", "head_sha": "ci/x"}]})), 0)
    expect("remote: a scheduled file on main that is disabled",
           remote("o/r", fixture([reg(1, "a.yml", "disabled_manually")], main)), 1)
    expect("remote: a scheduled file on main never registered",
           remote("o/r", fixture([reg(3, "c.yml")], main)), 1)
    expect("remote: off-main workflow with no run to name its branch",
           remote("o/r", fixture([reg(1, "a.yml"), reg(2, "b.yml")], main)), 1)
    expect("remote: zero registered workflows", remote("o/r", fixture([], main)), 1)

    def broken(path):
        raise OSError("503")
    expect("remote: the API cannot be read", remote("o/r", broken), 1)

    if failures:
        print(f"check_scheduled_watched self-test: {len(failures)} failed: {failures}")
        return 1
    print("check_scheduled_watched self-test: 13 cases passed")
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=str(ROOT))
    parser.add_argument("--remote", action="store_true")
    parser.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", ""))
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv[1:])
    if args.self_test:
        return self_test()
    if args.remote:
        if not args.repo or "GITHUB_TOKEN" not in os.environ:
            print("check_scheduled_watched --remote: needs --repo (or GITHUB_REPOSITORY) and GITHUB_TOKEN")
            return 1
        return remote(args.repo, gh_fetch)
    return local(args.root)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
