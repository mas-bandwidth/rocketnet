#!/usr/bin/env python3
"""Every workflow and composite action parses as strict YAML and has a workflow's shape.

GitHub rejects a workflow file whole when it is not valid YAML or not a
valid workflow: every push then makes a failed run with no jobs, and once
on main neither its schedule nor a dispatch can run it. The repository's
other checkers read workflows with their own lenient line parser, so a
file GitHub refuses can pass all of them. This one reads each file with
PyYAML's strict safe loader (duplicate keys refused, as GitHub refuses
them) and then checks the shape GitHub's schema requires:

  - a workflow is a mapping with `on` and a non-empty `jobs` mapping, and
    only the top-level keys a workflow may have;
  - each job is a mapping with only job keys, and either `runs-on` with a
    non-empty `steps` list, or `uses` (a reusable workflow) and no steps;
  - each step is a mapping with only step keys and exactly one of `run`
    and `uses`; a `name`, when present, is a string;
  - a composite action (.github/actions/*/action.yml) has `name`,
    `description` and `runs.using`; each composite step is shaped as above,
    and a `run` step names its `shell`, as GitHub requires there.

actionlint would check more, but the lint runner has no network to fetch
it and none is installed on the fleet; this is the strict parse plus the
shape. PyYAML is on every fleet runner (make docs needs it).

  python3 tools/check_workflow_yaml.py [--root DIR]
  python3 tools/check_workflow_yaml.py --self-test
"""

import sys
import tempfile
from pathlib import Path

try:
    import yaml
except ImportError:
    yaml = None

TOP_KEYS = {"name", "run-name", "on", "permissions", "env", "defaults", "concurrency", "jobs"}
JOB_KEYS = {"name", "needs", "permissions", "runs-on", "environment", "concurrency", "outputs", "env", "defaults",
            "if", "steps", "timeout-minutes", "strategy", "continue-on-error", "container", "services", "uses",
            "with", "secrets"}
STEP_KEYS = {"id", "if", "name", "uses", "run", "working-directory", "shell", "with", "env", "continue-on-error",
             "timeout-minutes"}
ACTION_KEYS = {"name", "author", "description", "inputs", "outputs", "runs", "branding"}


def strict_loader():
    class StrictLoader(yaml.SafeLoader):
        pass

    def mapping(loader, node, deep=False):
        seen = {}
        for key_node, _ in node.value:
            key = loader.construct_object(key_node, deep=deep)
            if key in seen:
                raise yaml.constructor.ConstructorError(
                    "while constructing a mapping", node.start_mark,
                    f"duplicate key {key!r} (first at line {seen[key] + 1})", key_node.start_mark)
            seen[key] = key_node.start_mark.line
        return loader.construct_mapping(node, deep=deep)

    StrictLoader.add_constructor(yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, mapping)
    return StrictLoader


def key_name(key):
    # YAML 1.1 reads a bare `on` key as the boolean true
    return "on" if key is True else key


def check_steps(where, steps, composite):
    found = []
    if not isinstance(steps, list) or not steps:
        return [f"{where}: steps is not a non-empty list"]
    for index, step in enumerate(steps, 1):
        at = f"{where} step {index}"
        if not isinstance(step, dict):
            found.append(f"{at}: is not a mapping")
            continue
        label = step.get("name", step.get("uses", step.get("run", "")))
        at = f"{at} ({str(label).splitlines()[0][:60] if label else 'unnamed'})"
        for key in step:
            if key not in STEP_KEYS:
                found.append(f"{at}: unknown step key {key!r}")
        if ("run" in step) == ("uses" in step):
            found.append(f"{at}: needs exactly one of run and uses")
        if "name" in step and not isinstance(step["name"], str):
            found.append(f"{at}: name is not a string")
        if composite and "run" in step and "shell" not in step:
            found.append(f"{at}: a composite action's run step must name its shell")
    return found


def check_workflow(rel, data):
    if not isinstance(data, dict):
        return [f"{rel}: is not a mapping"]
    found = []
    keys = {key_name(k) for k in data}
    for key in sorted(keys - TOP_KEYS, key=str):
        found.append(f"{rel}: unknown top-level key {key!r}")
    if "on" not in keys:
        found.append(f"{rel}: has no `on`")
    jobs = data.get("jobs")
    if not isinstance(jobs, dict) or not jobs:
        found.append(f"{rel}: has no jobs mapping")
        return found
    for job_id, job in jobs.items():
        where = f"{rel} job {job_id}"
        if not isinstance(job, dict):
            found.append(f"{where}: is not a mapping")
            continue
        for key in job:
            if key not in JOB_KEYS:
                found.append(f"{where}: unknown job key {key!r}")
        if "uses" in job:
            if "steps" in job or "runs-on" in job:
                found.append(f"{where}: a reusable-workflow job (uses) must have neither runs-on nor steps")
            continue
        if "runs-on" not in job:
            found.append(f"{where}: has no runs-on")
        found += check_steps(where, job.get("steps"), False)
    return found


def check_action(rel, data):
    if not isinstance(data, dict):
        return [f"{rel}: is not a mapping"]
    found = []
    for key in data:
        if key not in ACTION_KEYS:
            found.append(f"{rel}: unknown action key {key!r}")
    for key in ("name", "description", "runs"):
        if key not in data:
            found.append(f"{rel}: has no {key}")
    runs = data.get("runs")
    if isinstance(runs, dict):
        if "using" not in runs:
            found.append(f"{rel}: runs has no using")
        if runs.get("using") == "composite":
            found += check_steps(f"{rel} runs", runs.get("steps"), True)
    elif runs is not None:
        found.append(f"{rel}: runs is not a mapping")
    return found


def files(root):
    workflows = sorted(p for p in (root / ".github" / "workflows").glob("*") if p.suffix in (".yml", ".yaml"))
    actions = sorted(p for p in (root / ".github" / "actions").glob("*/action.y*ml"))
    return workflows, actions


def problems(root):
    if yaml is None:
        return ["PyYAML is not installed: the strict parse cannot run (python3 -m pip install PyYAML)"], 0
    workflows, actions = files(root)
    if not workflows:
        return [f"no workflow file under {root / '.github' / 'workflows'}: nothing was checked"], 0
    loader = strict_loader()
    found = []
    for path, check in [(p, check_workflow) for p in workflows] + [(p, check_action) for p in actions]:
        rel = path.relative_to(root).as_posix()
        try:
            data = yaml.load(path.read_text(encoding="utf-8"), Loader=loader)
        except yaml.YAMLError as error:
            mark = getattr(error, "problem_mark", None)
            line = f":{mark.line + 1}" if mark else ""
            problem = getattr(error, "problem", None) or str(error).splitlines()[0]
            found.append(f"{rel}{line}: not valid YAML ({problem}); GitHub rejects the whole file")
            continue
        found += check(rel, data)
    return found, len(workflows) + len(actions)


GOOD = """\
name: good
on:
  push:
  workflow_dispatch:
jobs:
  build:
    runs-on: [self-hosted, Linux]
    steps:
      - uses: actions/checkout@0000000000000000000000000000000000000000
      - name: 'Self-test: quoted is fine'
        run: echo ok
  call:
    uses: ./.github/workflows/other.yml
"""

ACTION = """\
name: cache
description: a composite action
runs:
  using: composite
  steps:
    - name: say
      shell: bash
      run: echo ok
"""


def self_test():
    failures = 0

    def case(name, workflow, want, action=ACTION):
        nonlocal failures
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / ".github" / "workflows").mkdir(parents=True)
            (root / ".github" / "actions" / "cache").mkdir(parents=True)
            (root / ".github" / "workflows" / "w.yml").write_text(workflow)
            (root / ".github" / "actions" / "cache" / "action.yml").write_text(action)
            found, _ = problems(root)
            ok = (not found) if want is None else any(want in f for f in found)
            print(f"{'ok' if ok else 'WRONG'}: {name}: {found[0] if found else 'clean'}")
            failures += 0 if ok else 1

    case("green: a quoted name with ': ', a reusable-workflow job, a composite action", GOOD, None)
    case("red: an unquoted step name holding ': ' (nightly-differential.yml:74 before cbb5ec89a)",
         GOOD.replace("'Self-test: quoted is fine'", "Self-test: the gate passes stock and fails on every plant"),
         "w.yml:10: not valid YAML (mapping values are not allowed here)")
    case("red: a duplicate key", GOOD.replace("    runs-on: [self-hosted, Linux]\n",
                                                "    runs-on: [self-hosted, Linux]\n    runs-on: [self-hosted]\n"),
         "duplicate key 'runs-on'")
    case("red: a tab in the indentation", GOOD.replace("        run: echo ok", "\trun: echo ok"), "not valid YAML")
    case("red: a step with run and uses", GOOD.replace("        run: echo ok", "        run: echo ok\n        uses: x/y@v1"),
         "needs exactly one of run and uses")
    case("red: a misspelt job key", GOOD.replace("    runs-on:", "    runs_on:"), "unknown job key 'runs_on'")
    case("red: no jobs", "name: x\non: push\n", "has no jobs mapping")
    case("red: no on", GOOD.replace("on:\n  push:\n  workflow_dispatch:\n", ""), "has no `on`")
    case("red: a composite run step without its shell", GOOD, "must name its shell",
         action=ACTION.replace("      shell: bash\n", ""))
    with tempfile.TemporaryDirectory() as tmp:
        found, _ = problems(Path(tmp))
        ok = any("no workflow file" in f for f in found)
        print(f"{'ok' if ok else 'WRONG'}: red: no workflow files: {found[0] if found else 'clean'}")
        failures += 0 if ok else 1
    print("check_workflow_yaml self-test: " + ("PASS" if failures == 0 else f"FAIL ({failures})"))
    return 1 if failures else 0


def main(argv):
    if argv[1:] == ["--self-test"]:
        return self_test()
    root = Path(__file__).resolve().parents[1]
    if argv[1:2] == ["--root"] and len(argv) == 3:
        root = Path(argv[2])
    elif argv[1:]:
        print(__doc__, file=sys.stderr)
        return 2
    found, count = problems(root)
    for line in found:
        print(f"FAIL: {line}", file=sys.stderr)
    if found:
        return 1
    print(f"OK: {count} workflow and action files parse as strict YAML and have a workflow's shape")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
