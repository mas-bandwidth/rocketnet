#!/usr/bin/env python3
"""Every `run:` step runs under a shell that stops on the first failure.

A step's shell is the first of: the step's `shell:`, the job's
`defaults.run.shell`, the workflow's `defaults.run.shell`, the runner's own
default (`bash -e {0}` on Linux and macOS: no pipefail, no -u; pwsh on
Windows). Each step is judged by the rule of the shell it gets:

  bash   the shell line carries -e, -u and -o pipefail (the workflows'
         defaults: `bash --noprofile --norc -euo pipefail {0}`), or the
         script's first command is `set -euo pipefail` (the explicit
         equivalent, used under `shell: bash` where a job's default is pwsh).
         On Windows only the `bash` keyword is accepted (GitHub maps it to
         Git Bash; a bash command line is looked up on PATH, where WSL's bash
         may come first), with `set -euo pipefail` as the first command.
  pwsh   GitHub's wrapper stops on a failing cmdlet and exits with the last
         native command's code, and nothing else: a native command that fails
         before the last line is ignored. A pwsh step is one command (blank
         lines, comments and backtick continuations aside), or every native
         command line is followed by `if ($LASTEXITCODE -ne 0) { exit ... }`.
         Rather than guess which pwsh lines are native, a step of more than
         one command is refused unless each command line is followed by that
         check, a pwsh keyword, a brace, or is itself a cmdlet (Verb-Noun),
         assignment or such a check.
  cmd    one command: the wrapper exits with the last command's errorlevel.
  python and node: an uncaught error is a non-zero exit.
  sh, or anything else: refused.

On a job whose runner may be Windows (a runs-on naming windows, or a matrix
value that does), a step with no shell gets pwsh there; it is judged as pwsh
and, for the other rows, as the default bash.

Every bash step, and every tracked .sh file under .github, scripts and tools,
is also refused for `local|export|declare|readonly|typeset x=$(...)`: the
builtin's status replaces the command's, so -e never sees the failure. Split
the declaration from the assignment.

A run line that is only `*name` is refused: inside a block scalar (`run: |`)
a YAML alias is literal text, so the shell runs a command named `*name`
(exit 127). The alias must be the whole value: `run: *name` (2026-11-12: five
ci.yml timing steps were turned into this form and every Windows row failed).

Scanned: .github/workflows/*.yml|yaml and the run steps of
.github/actions/**/action.yml|yaml (a composite action's step must name its
shell). Zero workflow files, or zero run steps, is an error.

Usage: check_run_shell.py [--root DIR] | --self-test
"""

import argparse
import re
import shlex
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import workflow_jobs  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
STRICT = "bash --noprofile --norc -euo pipefail {0}"

MASK = re.compile(r"(?:^|[;{]|&&|\|\|)\s*(?:local|export|declare|readonly|typeset)(?:\s+-[A-Za-z]+)*\s+[A-Za-z_][A-Za-z0-9_]*=[\"']?\$\(")
SET_STRICT = re.compile(r"^set(?:\s+(?:-[A-Za-z]+|-o|pipefail|errexit|nounset))+\s*$")
PWSH_CHECK = re.compile(r"^if\s*\(\s*\$LASTEXITCODE\s*-ne\s*0\s*\)", re.I)
PWSH_SAFE = re.compile(r"^(?:[A-Z][a-z]+-[A-Z][A-Za-z]+\b|\$[\w:]+\s*[+\-]?=|[{}()]|\}|(?:if|else|elseif|foreach|for|while|try|catch|finally|throw|exit|return|param|function|switch)\b|#)", re.I)


def bash_flags(shell):
    """(e, u, pipefail) the shell line turns on, or None if it is not bash."""
    try:
        words = shlex.split(shell)
    except ValueError:
        return None
    if not words or Path(words[0]).name != "bash":
        return None
    e = u = pipefail = False
    i = 1
    while i < len(words):
        word = words[i]
        if word == "-o" and i + 1 < len(words):
            if words[i + 1] == "pipefail":
                pipefail = True
            elif words[i + 1] == "errexit":
                e = True
            elif words[i + 1] == "nounset":
                u = True
            i += 2
            continue
        if re.fullmatch(r"-[A-Za-z]+", word):
            e |= "e" in word
            u |= "u" in word
            if word.endswith("o") and i + 1 < len(words) and words[i + 1] == "pipefail":
                pipefail = True
                i += 1
        i += 1
    return e, u, pipefail


def script_sets_strict(script):
    """True if the first command of the script is a `set` turning on -e, -u and pipefail."""
    for line in script.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        m = SET_STRICT.match(line)
        if not m:
            return False
        flags = bash_flags("bash " + line[3:])
        return flags == (True, True, True)
    return False


def commands(script):
    """The command lines of a pwsh or cmd script: blank and comment lines dropped, continuations joined."""
    out, pending = [], ""
    for line in script.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#") or stripped.lower().startswith("rem "):
            continue
        pending = (pending + " " + stripped) if pending else stripped
        if pending.endswith("`") or pending.endswith("^"):
            pending = pending[:-1].rstrip()
            continue
        out.append(pending)
        pending = ""
    if pending:
        out.append(pending)
    return out


def pwsh_problem(script):
    lines = commands(script)
    if len(lines) <= 1:
        return None
    for index, line in enumerate(lines):
        if PWSH_SAFE.match(line) or PWSH_CHECK.match(line):
            continue
        following = lines[index + 1] if index + 1 < len(lines) else ""
        if PWSH_CHECK.match(following):
            continue
        if index == len(lines) - 1:
            continue  # the wrapper exits with the last command's code
        return (f"pwsh runs {len(lines)} commands and `{line[:60]}` is not followed by "
                "`if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }`: a failure there is ignored")
    return None


def judge(shell, script, where_os):
    """None if the step stops on its first failure under this shell, else the reason."""
    if shell is None:
        if where_os == "windows":
            shell = "pwsh"
        else:
            shell = "bash -e {0}"
    name = shell.split()[0] if shell.split() else ""
    if shell in ("pwsh", "powershell"):
        return pwsh_problem(script)
    if shell == "cmd":
        n = len(commands(script))
        return None if n <= 1 else f"cmd runs {n} commands; only the last one's errorlevel is the step's"
    if shell == "python" or name in ("python", "python3", "node"):
        return None
    if shell == "bash":
        if script_sets_strict(script):
            return None
        return ("`shell: bash` is -eo pipefail without -u; use the workflow default "
                f"`{STRICT}` or start the script with `set -euo pipefail`")
    flags = bash_flags(shell)
    if flags is not None and where_os == "windows":
        return (f"shell `{shell}` on Windows: a bash command line is looked up on PATH and may be WSL's "
                "bash; use `shell: bash` (Git Bash) with `set -euo pipefail` first, or pwsh")
    if flags is None:
        return f"shell `{shell}` is not bash, pwsh, cmd or python: nothing here stops on a failure"
    if flags == (True, True, True) or script_sets_strict(script):
        return None
    missing = [n for n, on in zip(("-e", "-u", "-o pipefail"), flags) if not on]
    return f"shell `{shell}` lacks {' '.join(missing)}"


def possible_os(job):
    """The set of {'windows', 'posix'} the job's runner may be.

    A runs-on list is one runner (all its labels); an expression may be either,
    read from the expression and the job's matrix: windows if either names
    windows, posix if either names linux, macos or ubuntu, or names no windows.
    """
    runs_on = job.get("runs-on")
    if isinstance(runs_on, dict):
        runs_on = runs_on.get("labels") or runs_on.get("group") or ""
    text = " ".join(map(str, runs_on)) if isinstance(runs_on, list) else str(runs_on or "")
    windows = re.compile(r"windows", re.I)
    if "${{" not in text:
        return {"windows"} if windows.search(text) else {"posix"}
    text += " " + repr(((job.get("strategy") or {}).get("matrix")) or {})
    found = set()
    if windows.search(text):
        found.add("windows")
    if re.search(r"linux|macos|ubuntu", text, re.I) or not windows.search(text):
        found.add("posix")
    return found


def defaults_shell(node):
    return (((node or {}).get("defaults") or {}).get("run") or {}).get("shell")


def mask_lines(text):
    return [(n, line) for n, line in enumerate(text.splitlines(), 1) if MASK.search(line)]


ALIAS_LINE = re.compile(r"^\s*\*[A-Za-z_][A-Za-z0-9_-]*\s*$")


def alias_lines(text):
    """Lines of a run script that are only `*name`: an alias written inside a
    block scalar, which YAML keeps as literal text."""
    return [(n, line) for n, line in enumerate(text.splitlines(), 1) if ALIAS_LINE.match(line)]


def alias_problems(label, script):
    return [f"{label}: run line {n}: `{line.strip()}` is a YAML alias inside a block scalar: literal text the "
            f"shell runs as a command (exit 127); write `run: {line.strip()}`" for n, line in alias_lines(script)]


def check_workflow(path, rel, problems):
    data = workflow_jobs.parse(path.read_text(encoding="utf-8"), rel)
    steps = 0
    wf_shell = defaults_shell(data)
    for job_id, job in (data.get("jobs") or {}).items():
        job = job or {}
        job_shell = defaults_shell(job) or wf_shell
        oses = possible_os(job)
        for index, step in enumerate(job.get("steps") or []):
            if not isinstance(step, dict) or "run" not in step:
                continue
            steps += 1
            label = f"{rel}: {job_id}: {step.get('name') or step.get('id') or f'step {index + 1}'}"
            script = str(step.get("run") or "")
            shell = step.get("shell") or job_shell
            problems.extend(alias_problems(label, script))
            for os_name in sorted(oses):
                reason = judge(shell, script, os_name)
                if reason:
                    where = f" (on {os_name})" if len(oses) > 1 else ""
                    problems.append(f"{label}{where}: {reason}")
            effective = shell or "bash -e {0}"
            if "pwsh" not in effective and "powershell" not in effective and effective != "cmd":
                for n, line in mask_lines(script):
                    problems.append(f"{label}: run line {n}: `{line.strip()[:70]}` masks the command's status "
                                    "behind the builtin's; declare first, assign on its own line")
    return steps


def check_action(path, rel, problems):
    data = workflow_jobs.parse(path.read_text(encoding="utf-8"), rel)
    steps = 0
    for index, step in enumerate(((data.get("runs") or {}).get("steps")) or []):
        if not isinstance(step, dict) or "run" not in step:
            continue
        steps += 1
        label = f"{rel}: {step.get('name') or f'step {index + 1}'}"
        shell = step.get("shell")
        script = str(step.get("run") or "")
        problems.extend(alias_problems(label, script))
        if not shell:
            problems.append(f"{label}: a composite action's run step names no shell")
            continue
        reason = judge(shell, script, "posix")
        if reason:
            problems.append(f"{label}: {reason}")
        for n, line in mask_lines(script):
            problems.append(f"{label}: run line {n}: `{line.strip()[:70]}` masks the command's status")
    return steps


def shell_files(root):
    found = []
    for base in (".github", "scripts", "tools"):
        directory = root / base
        if directory.is_dir():
            found += [p for p in directory.rglob("*.sh") if p.is_file()]
    return sorted(found)


def run(root):
    root = Path(root).resolve()
    workflows = root / ".github" / "workflows"
    files = sorted(workflows.glob("*.yml")) + sorted(workflows.glob("*.yaml")) if workflows.is_dir() else []
    if not files:
        print(f"check_run_shell: 0 workflow files under {root}: nothing was checked")
        return 1
    problems, steps = [], 0
    for path in files:
        steps += check_workflow(path, path.relative_to(root).as_posix(), problems)
    actions = root / ".github" / "actions"
    if actions.is_dir():
        for path in sorted(list(actions.rglob("action.yml")) + list(actions.rglob("action.yaml"))):
            steps += check_action(path, path.relative_to(root).as_posix(), problems)
    scripts = shell_files(root)
    for path in scripts:
        rel = path.relative_to(root).as_posix()
        for n, line in mask_lines(path.read_text(encoding="utf-8", errors="replace")):
            problems.append(f"{rel}:{n}: `{line.strip()[:70]}` masks the command's status behind the "
                            "builtin's; declare first, assign on its own line")
    if steps == 0:
        print(f"check_run_shell: {len(files)} workflow files but 0 run steps: nothing was checked")
        return 1
    for problem in problems:
        print(problem)
    print(f"check_run_shell: {len(files)} workflow files, {steps} run steps, {len(scripts)} shell files, "
          f"{len(problems)} refused")
    return 1 if problems else 0


# ---------------------------------------------------------------- self-test

GOOD = f"""on: push
defaults:
  run:
    shell: {STRICT}
jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - run: make
      - shell: bash
        run: |
          # strict by hand
          set -euo pipefail
          make test
  windows:
    runs-on: windows-latest
    defaults:
      run:
        shell: pwsh
    steps:
      - run: cmake --build build
      - run: |
          cmake --build build
          if ($LASTEXITCODE -ne 0) {{ exit $LASTEXITCODE }}
          Write-Host done
          ctest
      - shell: python
        run: print(1)
"""


def write(root, files):
    for rel, text in files.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")


def self_test():
    cases = [
        ("good tree", {".github/workflows/a.yml": GOOD}, 0, "0 refused"),
        ("no workflow files", {"README.md": "x\n"}, 1, "0 workflow files"),
        ("no run steps", {".github/workflows/a.yml": "on: push\njobs:\n  a:\n    runs-on: x\n    steps:\n      - uses: a/b@v1\n"},
         1, "0 run steps"),
        ("runner default bash -e", {".github/workflows/a.yml": GOOD.replace(f"    shell: {STRICT}\n", "    shell: bash -e {0}\n")},
         1, "lacks -u -o pipefail"),
        ("no defaults at all", {".github/workflows/a.yml": GOOD.replace(f"defaults:\n  run:\n    shell: {STRICT}\n", "")},
         1, "build: step 1: shell `bash -e {0}` lacks -u -o pipefail"),
        ("shell: bash without set -u", {".github/workflows/a.yml": GOOD.replace("set -euo pipefail", "set -eo pipefail")},
         1, "`shell: bash` is -eo pipefail without -u"),
        ("sh", {".github/workflows/a.yml": GOOD.replace("      - run: make\n", "      - run: make\n        shell: sh\n")},
         1, "shell `sh` is not bash"),
        ("pwsh native failure ignored", {".github/workflows/a.yml": GOOD.replace(
            "          if ($LASTEXITCODE -ne 0) {{ exit $LASTEXITCODE }}\n".replace("{{", "{").replace("}}", "}"), "")},
         1, "`cmake --build build` is not followed by"),
        ("windows default pwsh", {".github/workflows/a.yml": GOOD.replace("    defaults:\n      run:\n        shell: pwsh\n", "")},
         1, "windows: step 1: shell `bash --noprofile --norc -euo pipefail {0}` on Windows"),
        ("matrix windows row gets pwsh", {".github/workflows/a.yml": "on: push\njobs:\n  m:\n    strategy:\n      matrix:\n        os: [ubuntu-latest, windows-latest]\n    runs-on: ${{ matrix.os }}\n    steps:\n      - run: |\n          a\n          b\n"},
         1, "m: step 1 (on windows): pwsh runs 2 commands"),
        ("local masks", {".github/workflows/a.yml": GOOD.replace("          make test\n", "          f() { local x=$(false); }\n          make test\n")},
         1, "masks the command's status"),
        ("export masks in a script", {".github/workflows/a.yml": GOOD, "scripts/x.sh": "#!/bin/bash\nexport A=\"$(false)\"\n"},
         1, "scripts/x.sh:2: `export A=\"$(false)\"` masks"),
        ("composite action without shell", {".github/workflows/a.yml": GOOD,
                                            ".github/actions/c/action.yml": "runs:\n  using: composite\n  steps:\n    - run: make\n"},
         1, "names no shell"),
        ("composite action with shell: bash", {".github/workflows/a.yml": GOOD,
                                               ".github/actions/c/action.yml": "runs:\n  using: composite\n  steps:\n    - run: make\n      shell: bash\n"},
         1, "`shell: bash` is -eo pipefail without -u"),
        ("cmd two commands", {".github/workflows/a.yml": GOOD.replace("      - shell: python\n        run: print(1)\n",
                                                                       "      - shell: cmd\n        run: |\n          a\n          b\n")},
         1, "cmd runs 2 commands"),
        ("alias inside a block scalar", {".github/workflows/a.yml": GOOD.replace(
            "      - run: make\n",
            "      - run: &timing |\n          set -euo pipefail\n          make\n"
            "      - shell: bash\n        run: |\n          set -euo pipefail\n          *timing\n")},
         1, "`*timing` is a YAML alias inside a block scalar"),
        ("alias as the whole value", {".github/workflows/a.yml": GOOD.replace(
            "      - run: make\n",
            "      - run: &timing |\n          set -euo pipefail\n          make\n      - run: *timing\n")},
         0, "0 refused"),
    ]
    import contextlib
    import io
    failed = 0
    for name, files, want_code, want_text in cases:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write(root, files)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = run(root)
            text = out.getvalue()
        ok = code == want_code and want_text in text
        print(f"{'ok  ' if ok else 'FAIL'} {name}: exit {code}")
        if not ok:
            failed += 1
            print(text)
    print(f"check_run_shell self-test: {len(cases)} cases, {failed} failed")
    return 1 if failed else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", default=str(ROOT))
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    return run(args.root)


if __name__ == "__main__":
    sys.exit(main())
