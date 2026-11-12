#!/usr/bin/env python3
"""Planted violations: every check is shown to go red on the violation it exists to catch.

A check counts only when it has been shown to FAIL and shown to have RUN. A
check's own self-test proves the tool; this proves the wiring: for each row of
tools/planted_violations.tsv it makes a fresh scratch checkout of HEAD, plants
one violation, runs the exact command the workflow step runs, and requires a
nonzero exit AND output matching the row's red regex. The scratch checkout is
then removed, so nothing planted survives.

Each row ends in exactly one line:

  PLANT-RED <id> :: <the red line>     the check caught its plant (good)
  PLANT-GREEN <id> (the check did not catch its plant)
  PLANT-BROKEN <id> (the plant did not apply | setup failed | ...)
  PLANT-HUNG <id> (no verdict within --row-timeout)
  PLANT-SKIP <id> (hardware: ...)      a row for a check that needs hardware
                                       no fleet label has; listed, never run

and the table as a whole is judged against the inventory:

  PLANT-MISSING <workflow>:<job>       a check with no table row
  PLANT-MISSING checker:<entry>        a tools/checkers.txt entry no row runs
  PLANT-STALE <id> (...)               a row naming a workflow, job or step that
                                       does not exist, or a command that is not
                                       the step's own text

The inventory is every job id of every workflow in .github/workflows that runs
per commit (on: push or pull_request) or on a schedule (on: schedule), except
reusable-call jobs (`uses:`), whose verdicts are the called workflow's jobs;
plus every entry of tools/checkers.txt when that file exists (first
whitespace-separated field of each non-comment line; a row covers an entry when
its command or setup contains the entry's text, or the entry's registered
self-test command, the line's second tab-separated field, whitespace collapsed:
a checker whose one CI step is its self-test is run by a row that runs that
self-test with the checker planted broken). Several rows may cover one job (one per
step that is a check); a job needs at least one.

Wiring is checked, not assumed (--check-table, and before every run):
  - the row's step is a step `name:` of that job (or `-` for any step), and the
    row's command, with whitespace collapsed, occurs in that step's `run:` text,
    where a `${{ ... }}` in the step text stands for any value the row fills in;
  - when the job's runs-on names exactly one of Linux, macOS, Windows, the row's
    os is that one.

Table: tab separated, a header line, `#` comments. Columns:

  id       unique row id, conventionally <workflow stem>/<job id>[/<what>]
  workflow file name under .github/workflows (ci.yml)
  job      job id in that file
  step     the step's `name:` whose verdict this is, or `-`
  os       linux | macos | windows (which nightly leg runs the row)
  skip     `-`, or `hardware: <what>` for a check that needs hardware absent
           from every fleet label (an owner decision; keep these few)
  plant    one of:
             sed:<path>:s<d>REGEX<d>REPLACEMENT<d>[g]   python re.sub on one
                file; must change it (else PLANT-BROKEN)
             patch:<file under tools/planted/>          `git apply` of a patch
             write:<path>:<content>                     create or overwrite a
                file; `\\n` in content is a newline
           several plants join with ` && `
  setup    the earlier steps of the job the command needs (configure, build),
           bash; `-` for none. Run after the plant; its failure is
           PLANT-BROKEN, never red.
  command  the step's own command (see wiring); `\\n` is a newline
  red      python regex the command's output must contain
  needs    `-`, or what the row needs from the machine that runs it, which the
           fleet runner of its job has and a bench may not; comma separated:
             env:NAME      the environment variable is set and not empty
             file:PATH     the path exists (~ and $VAR expanded)
             cmd:NAME      the program is on PATH
             quiet:F       the host's 1-minute load is at most F times the
                           CPUs this run may use (nproc); the row runs after
                           every other row, alone, and waits up to
                           QUIET_WAIT seconds for the load to come down
           A row whose need is not met is not run: PLANT-UNMET <id> (needs
           <what>: <why>), and the run exits 1 (the check was not shown red).

Commands run under `bash -euo pipefail` in the scratch checkout's root with
ROCKETNET_PLANTED=1 set. Output after setup is what the red regex is matched
against.

Usage:
  planted_violations.py [--os linux|macos|windows] [--only id,id] [--jobs N]
                        [--row-timeout SECONDS] [--keep]
  planted_violations.py --check-table     inventory/table consistency only (no plant runs)
  planted_violations.py --list            the inventory, one check per line
  planted_violations.py --self-test       fixtures, no build

Exit 0 when every run row is RED and the table is complete; 1 on any GREEN,
BROKEN, HUNG, UNMET, MISSING or STALE, or when zero rows ran; 2 for a table that
does not parse.
"""

import argparse
import concurrent.futures
import os
import platform
import queue
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import workflow_jobs  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
TABLE = "tools/planted_violations.tsv"
CHECKERS = "tools/checkers.txt"
PLANTED = "tools/planted"
COLUMNS = ["id", "workflow", "job", "step", "os", "skip", "plant", "setup", "command", "red", "needs"]
NEED_KINDS = ("env", "file", "cmd", "quiet")
# a quiet row waits this long for the host's load to come down before it is UNMET
QUIET_WAIT = 900
QUIET_POLL = 15
OSES = ("linux", "macos", "windows")
MARKER = "@@PLANTED-VIOLATIONS-SETUP-DONE@@"
RUNS_ON_OS = {"Linux": "linux", "macOS": "macos", "Windows": "windows"}


class TableError(Exception):
    pass


# ---------------------------------------------------------------- table

ESCAPE = re.compile(r"\\(\\|n|t|x[0-9A-Fa-f]{2})")


def unescape(text):
    """A table cell's text: \\n newline, \\t tab, \\xHH that byte, \\\\ one backslash.
    \\xHH lets a plant spell a word the tree's own checkers refuse without the table
    holding it."""
    def one(m):
        code = m.group(1)
        if code == "\\":
            return "\\"
        if code == "n":
            return "\n"
        if code == "t":
            return "\t"
        return chr(int(code[1:], 16))
    return ESCAPE.sub(one, text)


def load_table(path):
    rows = []
    seen = set()
    header = None
    for number, line in enumerate(Path(path).read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        fields = line.split("\t")
        if header is None:
            if fields != COLUMNS:
                raise TableError(f"{path}:{number}: header must be {' '.join(COLUMNS)}")
            header = fields
            continue
        if len(fields) != len(COLUMNS):
            raise TableError(f"{path}:{number}: {len(fields)} fields, want {len(COLUMNS)}")
        row = dict(zip(COLUMNS, fields))
        row["line"] = number
        if row["id"] in seen:
            raise TableError(f"{path}:{number}: duplicate id {row['id']}")
        seen.add(row["id"])
        if row["os"] not in OSES:
            raise TableError(f"{path}:{number}: os {row['os']!r} is not one of {', '.join(OSES)}")
        if row["skip"] != "-" and not row["skip"].startswith("hardware: "):
            raise TableError(f"{path}:{number}: skip must be '-' or 'hardware: <what>' "
                             "(only hardware absent from every fleet label excuses a row)")
        for column in ("plant", "command", "red"):
            if row[column] in ("", "-"):
                raise TableError(f"{path}:{number}: {column} is empty")
        try:
            re.compile(row["red"])
        except re.error as error:
            raise TableError(f"{path}:{number}: red regex: {error}") from error
        for plant in split_plants(row["plant"]):
            parse_plant(plant, f"{path}:{number}")
        row["_needs"] = parse_needs(row["needs"], f"{path}:{number}")
        rows.append(row)
    if header is None:
        raise TableError(f"{path}: no header line")
    return rows


def parse_needs(text, where):
    """[(kind, argument)] of a needs cell; '-' is none."""
    if text == "-":
        return []
    needs = []
    for item in (part.strip() for part in text.split(",")):
        kind, sep, argument = item.partition(":")
        if not sep or kind not in NEED_KINDS or not argument:
            raise TableError(f"{where}: need {item!r} is not one of " + ", ".join(k + ":..." for k in NEED_KINDS))
        if kind == "quiet":
            try:
                if not 0 < float(argument) <= 4:
                    raise ValueError
            except ValueError:
                raise TableError(f"{where}: quiet:{argument} wants a load fraction in (0, 4]") from None
        needs.append((kind, argument))
    return needs


def usable_cpus():
    """The CPUs this run may use (what nproc prints): the affinity mask where there is one."""
    if hasattr(os, "sched_getaffinity"):
        return len(os.sched_getaffinity(0))
    return os.cpu_count() or 1


def unmet_need(needs, environ=None, load=None, cpus=usable_cpus, sleep=None, clock=None):
    """None when every need is met, else '<kind>:<argument>: <why>' for the first that is not."""
    import time
    environ = os.environ if environ is None else environ
    load = load or (lambda: os.getloadavg()[0])
    sleep = sleep or time.sleep
    clock = clock or time.monotonic
    for kind, argument in needs:
        what = f"{kind}:{argument}"
        if kind == "env" and not environ.get(argument):
            return f"{what}: {argument} is not set"
        if kind == "file":
            path = os.path.expanduser(os.path.expandvars(argument))
            if not os.path.exists(path):
                return f"{what}: {path} does not exist on {platform.node()}"
        if kind == "cmd" and shutil.which(argument, path=environ.get("PATH")) is None:
            return f"{what}: {argument} is not on PATH on {platform.node()}"
        if kind == "quiet":
            limit = float(argument) * cpus()
            start = clock()
            while True:
                now = load()
                if now <= limit:
                    break
                if clock() - start >= QUIET_WAIT:
                    return (f"{what}: host load {now:.2f} over {limit:g} ({argument} x {cpus()} CPUs) on "
                            f"{platform.node()} after waiting {QUIET_WAIT} s")
                sleep(QUIET_POLL)
    return None


def split_plants(text):
    return [p.strip() for p in text.split(" && ") if p.strip()]


def parse_plant(plant, where):
    kind, _, rest = plant.partition(":")
    if kind == "sed":
        target, _, expr = rest.partition(":")
        if not target or len(expr) < 4 or expr[0] != "s":
            raise TableError(f"{where}: sed plant needs sed:<path>:s/REGEX/REPL/[g]")
        delim = expr[1]
        parts = split_unescaped(expr[2:], delim)
        if len(parts) != 3 or parts[2] not in ("", "g"):
            raise TableError(f"{where}: sed plant {plant!r}: want s{delim}REGEX{delim}REPL{delim}[g]")
        try:
            re.compile(parts[0], re.M)
        except re.error as error:
            raise TableError(f"{where}: sed plant regex: {error}") from error
        return ("sed", target, parts[0], unescape(parts[1].replace("\\" + delim, delim)), parts[2] == "g")
    if kind == "patch":
        if not rest:
            raise TableError(f"{where}: patch plant needs a file under {PLANTED}/")
        return ("patch", rest)
    if kind == "write":
        target, sep, content = rest.partition(":")
        if not target or not sep:
            raise TableError(f"{where}: write plant needs write:<path>:<content>")
        return ("write", target, unescape(content))
    raise TableError(f"{where}: unknown plant kind {kind!r} (sed, patch, write)")


def split_unescaped(text, delim):
    parts, current, i = [], "", 0
    while i < len(text):
        if text[i] == "\\" and i + 1 < len(text) and text[i + 1] == delim:
            current += "\\" + delim
            i += 2
            continue
        if text[i] == delim:
            parts.append(current)
            current = ""
        else:
            current += text[i]
        i += 1
    parts.append(current)
    return parts


# ---------------------------------------------------------------- inventory

def collapse(text):
    return " ".join(str(text).replace("\\\n", " ").split())


def load_workflows(root):
    found = {}
    directory = Path(root) / ".github" / "workflows"
    for path in sorted(list(directory.glob("*.yml")) + list(directory.glob("*.yaml"))):
        found[path.name] = workflow_jobs.Workflow(path)
    return found


def counted(workflow):
    return any(t in workflow.triggers for t in ("push", "pull_request", "schedule"))


def inventory(root, workflows=None):
    """[(key, kind)] where kind is 'job' or 'checker'."""
    workflows = workflows if workflows is not None else load_workflows(root)
    keys = []
    for name, workflow in workflows.items():
        if not counted(workflow):
            continue
        for job in workflow.jobs:
            if job.uses:
                continue
            keys.append((f"{name}:{job.id}", "job"))
    for entry in checker_selftests(root):
        keys.append((f"checker:{entry}", "checker"))
    return keys


def checker_selftests(root):
    """{entry: registered self-test command, collapsed ('' when the line has none)} for
    every tools/checkers.txt line, in file order; {} when the file does not exist."""
    found = {}
    checkers = Path(root) / CHECKERS
    if checkers.exists():
        for line in checkers.read_text(encoding="utf-8").splitlines():
            if not line.strip() or line.strip().startswith("#"):
                continue
            fields = line.split("\t")
            selftest = collapse(fields[1]) if len(fields) > 1 and fields[1].strip() != "-" else ""
            found[line.split()[0]] = selftest
    return found


def step_tokens(run_text):
    """The collapsed step text as characters, each ${{ ... }} one wildcard (None)."""
    tokens = []
    for piece in re.split(r"(\$\{\{.*?\}\})", collapse(run_text)):
        if piece.startswith("${{"):
            tokens.append(None)
        else:
            tokens.extend(piece)
    return tokens


def is_step_text(run_text, command):
    """True when command is a contiguous slice of the collapsed step text, in which a
    ${{ ... }} stands for any value of one or more characters (the row fills it in)."""
    if not command:
        return False
    if command in collapse(run_text):
        return True
    tokens = step_tokens(run_text)
    for start, token in enumerate(tokens):
        if token is not None and token != command[0]:
            continue
        states = {(start, 0)}
        seen = set()
        while states:
            following = set()
            for t, c in states:
                if c == len(command):
                    return True
                if t >= len(tokens) or (t, c) in seen:
                    continue
                seen.add((t, c))
                if tokens[t] is None:
                    following.add((t, c + 1))       # the value goes on
                    following.add((t + 1, c + 1))   # the value ends here
                elif tokens[t] == command[c]:
                    following.add((t + 1, c + 1))
            states = following
    return False


def runs_on_os(body):
    text = str(body.get("runs-on", ""))
    hits = {os_name for label, os_name in RUNS_ON_OS.items() if label in text}
    return hits.pop() if len(hits) == 1 else None


def judge_table(root, rows, workflows=None):
    """Lines for every MISSING and STALE problem; empty when the table is complete and true."""
    workflows = workflows if workflows is not None else load_workflows(root)
    problems = []
    covered = set()
    for row in rows:
        where = row["id"]
        workflow = workflows.get(row["workflow"])
        if workflow is None:
            problems.append(f"PLANT-STALE {where} (workflow {row['workflow']} does not exist)")
            continue
        if not counted(workflow):
            problems.append(f"PLANT-STALE {where} ({row['workflow']} runs neither per commit nor on a schedule)")
            continue
        body = (workflow.data.get("jobs") or {}).get(row["job"])
        if not isinstance(body, dict):
            problems.append(f"PLANT-STALE {where} (job {row['job']} is not in {row['workflow']})")
            continue
        covered.add(f"{row['workflow']}:{row['job']}")
        want_os = runs_on_os(body)
        if want_os and want_os != row["os"]:
            problems.append(f"PLANT-STALE {where} (os {row['os']}, but the job runs on {want_os})")
        steps = [s for s in (body.get("steps") or []) if isinstance(s, dict) and s.get("run")]
        if row["step"] != "-":
            steps = [s for s in steps if str(s.get("name", "")) == row["step"]]
            if not steps:
                problems.append(f"PLANT-STALE {where} (no step named {row['step']!r} with a run: in "
                                f"{row['workflow']}:{row['job']})")
                continue
        command = collapse(unescape(row["command"]))
        matching = [s for s in steps if is_step_text(s["run"], command)]
        if not matching:
            problems.append(f"PLANT-STALE {where} (command is not the step's own text: {command[:80]!r})")
            continue
        # the step's environment, as the workflow gives it: workflow, job and step env (values
        # that are expressions are left unset), and the step's working-directory
        env = {}
        for scope in (workflow.data.get("env"), body.get("env"), matching[0].get("env")):
            if isinstance(scope, dict):
                for key, value in scope.items():
                    if value is not None and "${{" not in str(value):
                        env[str(key)] = str(value)
        row["_env"] = env
        row["_wd"] = matching[0].get("working-directory")
    selftests = checker_selftests(root)
    for key, kind in inventory(root, workflows):
        if kind == "job":
            if key not in covered:
                problems.append(f"PLANT-MISSING {key}")
        else:
            entry = key[len("checker:"):]
            selftest = selftests.get(entry, "")

            def runs(row):
                texts = (row["command"], row["setup"])
                if any(entry in text for text in texts):
                    return True
                return bool(selftest) and any(selftest in collapse(unescape(text)) for text in texts)

            if not any(runs(r) for r in rows):
                problems.append(f"PLANT-MISSING {key}")
    return problems


# ---------------------------------------------------------------- planting

def host_os():
    system = platform.system()
    return {"Linux": "linux", "Darwin": "macos", "Windows": "windows"}.get(system, system.lower())


def apply_plants(row, tree, source_root):
    for plant in split_plants(row["plant"]):
        parsed = parse_plant(plant, row["id"])
        if parsed[0] == "sed":
            _, target, regex, replacement, every = parsed
            path = tree / target
            if not path.is_file():
                return f"sed target {target} does not exist"
            text = path.read_text(encoding="utf-8", errors="surrogateescape")
            new, count = re.subn(regex, lambda _m: replacement, text, count=0 if every else 1, flags=re.M)
            if count == 0 or new == text:
                return f"sed {regex!r} matched nothing in {target}"
            path.write_text(new, encoding="utf-8", errors="surrogateescape")
        elif parsed[0] == "patch":
            patch = Path(source_root) / PLANTED / parsed[1]
            if not patch.is_file():
                return f"patch {PLANTED}/{parsed[1]} does not exist"
            result = subprocess.run(["git", "apply", "--whitespace=nowarn", str(patch)], cwd=tree,
                                    capture_output=True, text=True)
            if result.returncode != 0:
                return f"git apply {parsed[1]}: {result.stderr.strip()[:200]}"
        else:
            _, target, content = parsed
            path = tree / target
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
    return None


def scratch_checkout(root, path):
    """A fresh detached worktree of HEAD at path (a fixed slot path, so a compiler cache keyed
    on absolute paths hits from one row to the next); returns path."""
    remove_checkout(root, path)
    result = subprocess.run(["git", "worktree", "add", "--detach", "--quiet", str(path), "HEAD"],
                            cwd=root, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"git worktree add: {result.stderr.strip()}")
    return Path(path)


def runner_temp(tree):
    """A row's RUNNER_TEMP: a sibling of its scratch checkout (<slot>.runner-temp)."""
    tree = Path(tree)
    return tree.with_name(tree.name + ".runner-temp")


def remove_checkout(root, path):
    shutil.rmtree(runner_temp(Path(path).resolve()), ignore_errors=True)
    if Path(path).exists():
        subprocess.run(["git", "worktree", "remove", "--force", str(path)], cwd=root, capture_output=True)
        shutil.rmtree(path, ignore_errors=True)
    subprocess.run(["git", "worktree", "prune"], cwd=root, capture_output=True)


def run_script(script, tree, timeout):
    """(exit status or None on timeout, output). The whole process group dies on a timeout."""
    # each row gets its own runner temp and step summary, so parallel rows never share a
    # timing receipt; the workspace is the scratch checkout
    tree = Path(tree).resolve()  # the script runs inside the tree: every path it is given is absolute
    # beside the checkout, never inside it, as on a runner: a step that archives the workspace
    # (tar -C "$GITHUB_WORKSPACE") must not take in its own live work files under RUNNER_TEMP
    # (nightly-bigendian-system on bench-nyc-3: "overlay.qcow2: file changed as we read it")
    temp = runner_temp(tree)
    shutil.rmtree(temp, ignore_errors=True)
    temp.mkdir(parents=True)
    env = dict(os.environ, ROCKETNET_PLANTED="1", RUNNER_TEMP=str(temp), GITHUB_WORKSPACE=str(tree),
               GITHUB_STEP_SUMMARY=str(temp / "step-summary.md"),
               RUNNER_OS={"linux": "Linux", "macos": "macOS", "windows": "Windows"}.get(host_os(), "Linux"))
    # the files a step writes for the steps after it ($GITHUB_ENV, $GITHUB_OUTPUT, $GITHUB_PATH);
    # the row's script applies $GITHUB_ENV and $GITHUB_PATH after its setup, as the runner would
    for name, filename in (("GITHUB_ENV", "github-env"), ("GITHUB_OUTPUT", "github-output"),
                           ("GITHUB_PATH", "github-path")):
        (temp / filename).touch()
        env[name] = str(temp / filename)
    env.setdefault("GITHUB_RUN_ID", "0")
    env.setdefault("GITHUB_RUN_ATTEMPT", "1")
    env.setdefault("RUNNER_NAME", platform.node())
    kwargs = {}
    if os.name == "posix":
        kwargs["start_new_session"] = True
    proc = subprocess.Popen(["bash", "-euo", "pipefail", "-c", script], cwd=tree, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **kwargs)
    try:
        data, _ = proc.communicate(timeout=timeout)
        return proc.returncode, data.decode("utf-8", errors="replace")
    except subprocess.TimeoutExpired:
        if os.name == "posix":
            import signal
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        else:
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)], capture_output=True)
        data, _ = proc.communicate()
        return None, data.decode("utf-8", errors="replace")


# what the runner does between steps: lines a setup step appended to $GITHUB_PATH and
# $GITHUB_ENV (NAME=value form) reach the checked step
APPLY_STEP_FILES = (
    'while IFS= read -r planted_line; do if [ -n "$planted_line" ]; then PATH="$planted_line:$PATH"; fi; '
    'done < "$GITHUB_PATH"; export PATH\n'
    'while IFS= read -r planted_line; do case "$planted_line" in *=*) export "$planted_line";; esac; '
    'done < "$GITHUB_ENV"\n')

# a setup line that is exactly this separates two of the job's steps: what the first wrote to
# $GITHUB_ENV and $GITHUB_PATH reaches the second, and the second starts in the workspace, as on
# the runner (a `cd` in one step does not carry into the next)
STEP_BREAK = "# --- next step ---"
NEXT_STEP = APPLY_STEP_FILES + 'cd "$GITHUB_WORKSPACE"\n'


def setup_script(setup):
    """The setup cell as a script: each STEP_BREAK line applies the step files and returns to the
    workspace."""
    return "".join(NEXT_STEP if line == STEP_BREAK else line + "\n" for line in setup.split("\n"))


def run_row(row, root, path, timeout=None, keep=False, checkout=scratch_checkout):
    """(verdict, detail) for one row; verdict RED, GREEN, BROKEN or HUNG."""
    tree = checkout(root, path)
    try:
        problem = apply_plants(row, tree, root)
        if problem:
            return "BROKEN", f"the plant did not apply: {problem}"
        setup = "" if row["setup"] == "-" else unescape(row["setup"])
        env, wd = row.get("_env", {}), row.get("_wd")
        if any(".github/workflows/" in plant for plant in split_plants(row["plant"])):
            # the plant edits a workflow: the step runs with the planted workflow's env
            fresh = dict(row)
            judge_table(tree, [fresh])
            env, wd = fresh.get("_env", env), fresh.get("_wd", wd)
        exports = "".join(f"export {k}={shlex.quote(v)}\n" for k, v in sorted(env.items())
                          if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", k))
        enter = f"cd {shlex.quote(str(wd))}\n" if wd and "${{" not in str(wd) else ""
        script = f"{exports}{setup_script(setup)}{APPLY_STEP_FILES}echo {MARKER}\n{enter}{unescape(row['command'])}\n"
        status, output = run_script(script, tree, timeout)
        if status is None:
            return "HUNG", f"no verdict within {timeout} s"
        if MARKER not in output:
            tail = " | ".join(output.strip().splitlines()[-3:])
            return "BROKEN", f"setup failed (exit {status}): {tail[:300]}"
        after = output.split(MARKER, 1)[1]
        match = re.search(row["red"], after, re.M)
        if status != 0 and match:
            line = after[:match.start()].rsplit("\n", 1)[-1] + after[match.start():].split("\n", 1)[0]
            return "RED", line.strip()[:300]
        tail = " | ".join(after.strip().splitlines()[-3:])
        why = "exit 0" if status == 0 else f"exit {status} but no line matches {row['red']!r}"
        return "GREEN", f"the check did not catch its plant: {why}; last lines: {tail[:300]}"
    finally:
        if not keep:
            remove_checkout(root, tree)


# ---------------------------------------------------------------- main

def say(line):
    """One verdict line, flushed at once so a long run's log shows each row as it lands."""
    print(line, flush=True)


def run(argv, root=ROOT, out=say, checkout=scratch_checkout, need_probes=None):
    need_probes = need_probes or {}
    parser = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    parser.add_argument("--table", default=None)
    parser.add_argument("--os", choices=OSES, default=None)
    parser.add_argument("--only", default=None, help="comma-separated row ids")
    parser.add_argument("--exclude", default=None, help="comma-separated row ids not to run (reported)")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--row-timeout", type=float, default=None)
    parser.add_argument("--keep", action="store_true")
    parser.add_argument("--scratch", default=None, help="directory for scratch checkouts")
    parser.add_argument("--check-table", action="store_true")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test(out)
    root = Path(root)
    if args.list:
        for key, _ in inventory(root):
            out(key)
        return 0
    table = Path(args.table) if args.table else root / TABLE
    try:
        rows = load_table(table)
    except (TableError, OSError) as error:
        out(f"planted-violations: {error}")
        return 2
    try:
        problems = judge_table(root, rows)
    except workflow_jobs.WorkflowError as error:
        out(f"planted-violations: a workflow did not parse: {error}")
        return 2
    for line in problems:
        out(line)
    if args.check_table:
        jobs = len([k for k, kind in inventory(root) if kind == "job"])
        if problems:
            out(f"planted-violations: table check: {len(problems)} problem(s) in {len(rows)} rows")
            return 1
        if not rows:
            out("planted-violations: table check: zero rows")
            return 1
        out(f"planted-violations: table check: ok ({len(rows)} rows cover {jobs} checks)")
        return 0
    want_os = args.os or host_os()
    selected = [r for r in rows if r["os"] == want_os]
    elsewhere = [r for r in rows if r["os"] != want_os]
    if args.only:
        names = set(n.strip() for n in args.only.split(",") if n.strip())
        unknown = names - {r["id"] for r in rows}
        for name in sorted(unknown):
            out(f"PLANT-STALE {name} (--only names no row)")
            problems.append(name)
        for row in [r for r in elsewhere if r["id"] in names]:
            out(f"PLANT-NOT-RUN {row['id']} (a {row['os']} row; this run is {want_os})")
        selected = [r for r in selected if r["id"] in names]
        elsewhere = [r for r in elsewhere if r["id"] in names]
    other = {name: sum(1 for r in elsewhere if r["os"] == name) for name in OSES if name != want_os}
    out(f"planted-violations: {len(selected)} {want_os} row(s) selected; not run here, being other systems' rows: "
        + ", ".join(f"{name} {count}" for name, count in other.items()))
    if args.exclude:
        dropped = set(n.strip() for n in args.exclude.split(",") if n.strip())
        for row in [r for r in selected if r["id"] in dropped]:
            out(f"PLANT-NOT-RUN {row['id']} (--exclude: not run here, so not shown red here)")
        selected = [r for r in selected if r["id"] not in dropped]
    skipped = [r for r in selected if r["skip"] != "-"]
    selected = [r for r in selected if r["skip"] == "-"]
    for row in skipped:
        out(f"PLANT-SKIP {row['id']} ({row['skip']})")
    base = Path(args.scratch).resolve() if args.scratch else Path(tempfile.mkdtemp(prefix="planted-violations-"))
    base.mkdir(parents=True, exist_ok=True)
    counts = {"RED": 0, "GREEN": 0, "BROKEN": 0, "HUNG": 0, "UNMET": 0}
    slots = queue.Queue()
    for number in range(max(1, args.jobs)):
        slots.put(base / f"slot-{number}")

    def in_a_slot(row):
        path = slots.get()
        try:
            missing = unmet_need(row.get("_needs", []), **need_probes)
            if missing:
                return "UNMET", f"needs {missing}"
            return run_row(row, root, path, args.row_timeout, args.keep, checkout)
        finally:
            slots.put(path)

    def report(row, verdict, detail):
        counts[verdict] += 1
        if verdict == "RED":
            out(f"PLANT-RED {row['id']} :: {detail}")
        else:
            out(f"PLANT-{verdict} {row['id']} ({detail})")

    # a row that needs a quiet host runs after every other row, one at a time
    quiet = [r for r in selected if any(kind == "quiet" for kind, _ in r.get("_needs", []))]
    shared = [r for r in selected if r not in quiet]
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        futures = {pool.submit(in_a_slot, row): row for row in shared}
        for future in concurrent.futures.as_completed(futures):
            row = futures[future]
            try:
                verdict, detail = future.result()
            except Exception as error:  # a scratch checkout that cannot be made is broken, not green
                verdict, detail = "BROKEN", f"scratch checkout failed: {error}"
            report(row, verdict, detail)
    for row in quiet:
        try:
            verdict, detail = in_a_slot(row)
        except Exception as error:  # a scratch checkout that cannot be made is broken, not green
            verdict, detail = "BROKEN", f"scratch checkout failed: {error}"
        report(row, verdict, detail)
    if not args.keep and not args.scratch:
        shutil.rmtree(base, ignore_errors=True)
    ran = sum(counts.values())
    out(f"planted-violations: os={want_os} rows run={ran} red={counts['RED']} green={counts['GREEN']} "
        f"broken={counts['BROKEN']} hung={counts['HUNG']} unmet={counts['UNMET']} skipped={len(skipped)} "
        f"table problems={len(problems)}")
    if ran == 0:
        out("planted-violations: zero rows ran: nothing was shown to fail")
        return 1
    if counts["GREEN"] or counts["BROKEN"] or counts["HUNG"] or counts["UNMET"] or problems:
        return 1
    return 0


# ---------------------------------------------------------------- self-test

def self_test(out=say):
    failures = []

    def expect(name, condition):
        if not condition:
            failures.append(name)
            out(f"self-test FAIL: {name}")

    with tempfile.TemporaryDirectory() as tmp:
        repo = Path(tmp) / "repo"
        (repo / ".github" / "workflows").mkdir(parents=True)
        (repo / "tools" / "planted").mkdir(parents=True)
        (repo / ".github" / "workflows" / "w.yml").write_text(
            "name: w\non:\n  push:\n    branches: [main]\njobs:\n"
            "  good:\n    runs-on: [self-hosted, Linux, X64, fleet]\n    steps:\n"
            "      - name: Check\n        run: sh check.sh data.txt\n"
            "  lazy:\n    runs-on: [self-hosted, Linux, X64, fleet]\n    steps:\n"
            "      - name: Lazy\n        run: sh lazy.sh\n"
            "  matrixed:\n    runs-on: [self-hosted, Linux, X64, fleet]\n    steps:\n"
            "      - name: Check ${{ matrix.part }}\n        run: |\n"
            "          sh check.sh ${{ matrix.part }}.txt\n"
            "  uncovered:\n    runs-on: [self-hosted, Linux, X64, fleet]\n    steps:\n"
            "      - run: echo ok\n"
            "  called:\n    uses: ./.github/workflows/other.yml\n",
            encoding="utf-8")
        (repo / ".github" / "workflows" / "manual.yml").write_text(
            "name: m\non:\n  workflow_dispatch:\njobs:\n  any:\n    runs-on: [self-hosted]\n"
            "    steps:\n      - run: echo ok\n", encoding="utf-8")
        (repo / "check.sh").write_text(
            "if grep -q BAD \"$1\"; then echo \"check: BAD found in $1\"; exit 1; fi\necho \"check: ok\"\n",
            encoding="utf-8")
        (repo / "lazy.sh").write_text("echo lazy: ok\n", encoding="utf-8")
        (repo / "data.txt").write_text("fine\n", encoding="utf-8")
        (repo / "a.txt").write_text("fine\n", encoding="utf-8")
        (repo / "tools" / "planted" / "bad.patch").write_text(
            "--- a/data.txt\n+++ b/data.txt\n@@ -1 +1 @@\n-something else\n+BAD\n", encoding="utf-8")
        for command in (["git", "init", "-q"], ["git", "add", "-A"],
                        ["git", "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "x"]):
            subprocess.run(command, cwd=repo, check=True, capture_output=True)
        header = "\t".join(COLUMNS)

        def table(*rows):
            path = Path(tmp) / "table.tsv"
            # a fixture row without its needs cell needs nothing
            path.write_text(header + "\n" + "".join("\t".join(list(r) + ["-"] * (len(COLUMNS) - len(r))) + "\n"
                                                    for r in rows), encoding="utf-8")
            return str(path)

        good = ["w/good", "w.yml", "good", "Check", "linux", "-", "sed:data.txt:s/fine/BAD/", "-",
                "sh check.sh data.txt", r"check: BAD found"]
        lazy = ["w/lazy", "w.yml", "lazy", "Lazy", "linux", "-", "sed:data.txt:s/fine/BAD/", "-",
                "sh lazy.sh", r"BAD found"]
        matrixed = ["w/matrixed", "w.yml", "matrixed", "Check ${{ matrix.part }}", "linux", "-",
                    "write:a.txt:BAD\\n", "-", "sh check.sh a.txt", r"check: BAD found in a\.txt"]
        uncovered = ["w/uncovered", "w.yml", "uncovered", "-", "linux", "-", "write:x:y", "-", "echo ok", "ok"]
        stale = ["w/gone", "w.yml", "gone", "-", "linux", "-", "write:x:y", "-", "echo ok", "ok"]
        noapply = ["w/noapply", "w.yml", "good", "Check", "linux", "-", "patch:bad.patch", "-",
                   "sh check.sh data.txt", r"BAD found"]
        noapply_sed = ["w/noapply-sed", "w.yml", "good", "Check", "linux", "-", "sed:data.txt:s/absent/BAD/",
                       "-", "sh check.sh data.txt", r"BAD found"]
        drifted = ["w/drifted", "w.yml", "good", "Check", "linux", "-", "sed:data.txt:s/fine/BAD/", "-",
                   "sh check.sh other.txt", r"BAD found"]
        wrong_os = ["w/wrong-os", "w.yml", "good", "Check", "macos", "-", "sed:data.txt:s/fine/BAD/", "-",
                    "sh check.sh data.txt", r"BAD found"]
        manual = ["m/any", "manual.yml", "any", "-", "linux", "-", "write:x:y", "-", "echo ok", "ok"]
        complete = [good, lazy, matrixed, uncovered]

        def go(rows, *extra, probes=None):
            lines = []
            code = run(["--table", table(*rows), "--os", "linux", "--jobs", "2", *extra], root=repo,
                       out=lines.append, need_probes=probes)
            return code, lines

        def has(lines, prefix):
            return any(line.startswith(prefix) for line in lines)

        # a check that catches its plant: RED, and the scratch tree is gone (no plant left behind)
        code, lines = go(complete, "--only", "w/good,w/matrixed")
        expect("catching check is PLANT-RED", has(lines, "PLANT-RED w/good :: check: BAD found in data.txt"))
        expect("matrix step filled in is PLANT-RED", has(lines, "PLANT-RED w/matrixed"))

        # a setup step's $GITHUB_ENV line reaches the checked step, as on the runner
        stepenv = ["w/stepenv", "w.yml", "matrixed", "Check ${{ matrix.part }}", "linux", "-",
                   "write:a.txt:BAD\\n", 'echo PLANTED_PART=a >> "$GITHUB_ENV"',
                   "sh check.sh ${PLANTED_PART}.txt", r"check: BAD found in a\.txt"]
        # with a relative --scratch, read against the caller's directory (not the repo's)
        here = os.getcwd()
        os.chdir(tmp)
        try:
            env_code, env_lines = go([good, lazy, matrixed, uncovered, stepenv], "--only", "w/stepenv",
                                     "--scratch", "relative-scratch")
        finally:
            os.chdir(here)
        expect("a setup step's GITHUB_ENV reaches the check (relative --scratch)",
               has(env_lines, "PLANT-RED w/stepenv") and env_code == 0)
        # a STEP_BREAK line hands one setup step's $GITHUB_ENV to the next setup step
        wrote = 'echo PLANTED_PART=a >> "$GITHUB_ENV"'
        needs = '[ "${PLANTED_PART:-}" = a ] || exit 3'
        broken_row = ["w/stepbreak", "w.yml", "matrixed", "Check ${{ matrix.part }}", "linux", "-",
                      "write:a.txt:BAD\\n", f"{wrote}\\n{needs}",
                      "sh check.sh ${PLANTED_PART}.txt", r"check: BAD found in a\.txt"]
        code_b, lines_b = go([good, lazy, matrixed, uncovered, broken_row], "--only", "w/stepbreak")
        expect("without a step break the next setup step does not see GITHUB_ENV (BROKEN)",
               has(lines_b, "PLANT-BROKEN w/stepbreak") and code_b == 1)
        broken_row[7] = f"{wrote}\\n{STEP_BREAK}\\n{needs}"
        code_b, lines_b = go([good, lazy, matrixed, uncovered, broken_row], "--only", "w/stepbreak")
        expect("a step break hands GITHUB_ENV to the next setup step (RED)",
               has(lines_b, "PLANT-RED w/stepbreak") and code_b == 0)
        # RUNNER_TEMP is beside the scratch checkout, never inside it (a step that archives the
        # workspace must not take in its own live files)
        slot = Path(tmp) / "slot-0"
        expect("RUNNER_TEMP is outside the checkout", runner_temp(slot).parent == slot.parent and runner_temp(slot) != slot)
        # a cd in one setup step does not carry into the next
        cd_row = list(broken_row)
        cd_row[0] = "w/stepcd"
        cd_row[7] = f"{wrote}\\nmkdir -p elsewhere\\ncd elsewhere\\n{STEP_BREAK}\\n[ -f check.sh ] || exit 3"
        code_b, lines_b = go([good, lazy, matrixed, uncovered, cd_row], "--only", "w/stepcd")
        expect("a step break returns to the workspace (RED)", has(lines_b, "PLANT-RED w/stepcd") and code_b == 0)
        expect("all red, complete table exits 0", code == 0)
        expect("the plant did not touch the real tree", (repo / "data.txt").read_text() == "fine\n")
        # a check that does not catch its plant
        code, lines = go(complete, "--only", "w/lazy")
        expect("lazy check is PLANT-GREEN", has(lines, "PLANT-GREEN w/lazy (the check did not catch its plant"))
        expect("a GREEN row exits 1", code == 1)
        # needs: a row whose need is not met is UNMET, never run, and the run exits 1; met, it runs
        needy = list(good) + ["env:PLANTED_VIOLATIONS_NEVER_SET"]
        needy[0] = "w/needy"
        code, lines = go([needy, lazy, matrixed, uncovered], "--only", "w/needy")
        expect("an unset env need is PLANT-UNMET and exits 1",
               has(lines, "PLANT-UNMET w/needy (needs env:PLANTED_VIOLATIONS_NEVER_SET: ") and code == 1
               and not has(lines, "PLANT-RED w/needy"))
        needy[10] = "file:" + str(repo / "check.sh") + ", cmd:sh"
        code, lines = go([needy, lazy, matrixed, uncovered], "--only", "w/needy")
        expect("met file and cmd needs run the row (RED)", has(lines, "PLANT-RED w/needy") and code == 0)
        clock = [0.0]
        busy = {"load": lambda: 9.0, "cpus": lambda: 4, "sleep": lambda s: clock.__setitem__(0, clock[0] + s),
                "clock": lambda: clock[0]}
        needy[10] = "quiet:0.5"
        code, lines = go([needy, lazy, matrixed, uncovered], "--only", "w/needy", probes=busy)
        expect("a busy host is quiet-UNMET after the wait",
               has(lines, f"PLANT-UNMET w/needy (needs quiet:0.5: host load 9.00 over 2 (0.5 x 4 CPUs)")
               and clock[0] >= QUIET_WAIT and code == 1)
        idle = dict(busy, load=lambda: 1.0)
        code, lines = go([needy, lazy, matrixed, uncovered], "--only", "w/needy", probes=idle)
        expect("a quiet host runs the quiet row (RED)", has(lines, "PLANT-RED w/needy") and code == 0)
        for bad in ("gpu:yes", "env:", "quiet:0", "quiet:x"):
            needy[10] = bad
            code, lines = go([needy, lazy, matrixed, uncovered], "--check-table")
            expect(f"need {bad!r} does not parse (exit 2)", code == 2)
        # a missing row (job with no row) is red even when every run row is red
        code, lines = go([good, lazy, matrixed], "--only", "w/good")
        expect("uncovered job is PLANT-MISSING", "PLANT-MISSING w.yml:uncovered" in lines)
        expect("a missing row exits 1", code == 1)
        expect("a reusable call is not inventoried", not any("w.yml:called" in line for line in lines))
        expect("a dispatch-only workflow is not inventoried", not any("manual.yml" in line for line in lines))
        # a stale row, a row for a dispatch-only workflow, a drifted command, a wrong os
        code, lines = go(complete + [stale, manual, drifted, wrong_os], "--check-table")
        expect("stale job row is PLANT-STALE", has(lines, "PLANT-STALE w/gone (job gone is not in w.yml)"))
        expect("dispatch-only workflow row is PLANT-STALE", has(lines, "PLANT-STALE m/any"))
        expect("drifted command is PLANT-STALE", has(lines, "PLANT-STALE w/drifted (command is not"))
        expect("wrong os is PLANT-STALE", has(lines, "PLANT-STALE w/wrong-os (os macos"))
        expect("table problems exit 1", code == 1)
        code, lines = go(complete, "--check-table")
        expect("complete true table passes --check-table", code == 0)
        # a plant that does not apply (patch and sed)
        code, lines = go(complete + [noapply, noapply_sed], "--only", "w/noapply,w/noapply-sed")
        expect("unapplicable patch is PLANT-BROKEN", has(lines, "PLANT-BROKEN w/noapply (the plant did not apply"))
        expect("sed matching nothing is PLANT-BROKEN", has(lines, "PLANT-BROKEN w/noapply-sed (the plant did not apply"))
        expect("a BROKEN row exits 1", code == 1)
        # zero rows run is red (an os with no rows), and an empty table is red
        code, lines = go(complete, "--os", "windows")
        expect("zero rows run exits 1", code == 1 and any("zero rows ran" in line for line in lines))
        code, lines = go([], "--check-table")
        expect("an empty table fails --check-table", code == 1)
        # a checkers.txt entry with no row is missing; a row whose command runs it covers it
        (repo / CHECKERS).write_text("# id\nlazy.sh  a lazy checker\nnever.sh a checker no row runs\n",
                                     encoding="utf-8")
        code, lines = go(complete, "--check-table")
        expect("checkers.txt entry with no row is PLANT-MISSING", "PLANT-MISSING checker:never.sh" in lines)
        expect("checkers.txt entry a row runs is covered", "PLANT-MISSING checker:lazy.sh" not in lines)
        # an entry whose row runs only its registered self-test (field 2) is covered; one whose
        # self-test no row runs is still missing
        (repo / CHECKERS).write_text(
            "# path\tself-test\tzero\tworkflow\tjob\tstep\n"
            "tools/judge.py\tsh   check.sh data.txt\t-\tw.yml\tgood\tCheck\n"
            "tools/alone.py\tsh alone-selftest.sh\t-\tw.yml\tgood\tCheck\n", encoding="utf-8")
        code, lines = go(complete, "--check-table")
        expect("checkers.txt entry whose self-test a row runs is covered",
               "PLANT-MISSING checker:tools/judge.py" not in lines)
        expect("checkers.txt entry whose self-test no row runs is PLANT-MISSING",
               "PLANT-MISSING checker:tools/alone.py" in lines)
        (repo / CHECKERS).unlink()
        # a table that does not parse exits 2
        bad = Path(tmp) / "bad.tsv"
        bad.write_text("id\tworkflow\n", encoding="utf-8")
        lines = []
        expect("unparsable table exits 2",
               run(["--table", str(bad), "--check-table"], root=repo, out=lines.append) == 2)
        bad.write_text(header + "\n" + "\t".join(good[:5] + ["because"] + good[6:]) + "\n", encoding="utf-8")
        expect("a skip without hardware: exits 2",
               run(["--table", str(bad), "--check-table"], root=repo, out=lines.append) == 2)
    if failures:
        out(f"planted-violations self-test: {len(failures)} failure(s)")
        return 1
    out("planted-violations self-test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(run(sys.argv[1:]))
